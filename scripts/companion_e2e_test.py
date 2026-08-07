#!/usr/bin/env python3
"""
End-to-end test harness for Companion Display Protocol v11, on real hardware.

Drives both halves of the device at once: BLE over the host's own radio, and
physical buttons over USB serial. That combination is what makes v6's most
important path testable at all — enrollment needs a CONFIRM press on the device,
which a BLE script cannot produce on its own.

The wire format lives in scripts/companion_protocol.py, shared with
scripts/push_companion_content.py. It used to be duplicated here, and the copy
froze at v6 while the pusher was kept current — this harness then refused to
start for five consecutive protocol versions ("Device speaks protocol v11, this
harness speaks v6") without anyone noticing, because it failed fast and quietly.
Do not reintroduce a local copy of any opcode, field id or frame layout.

Requires a firmware built with the serial test console:

    pio run -e test -t upload --upload-port /dev/cu.usbmodem21201

Then:

    python scripts/companion_e2e_test.py --port /dev/cu.usbmodem21201
    python scripts/companion_e2e_test.py --port ... --only enrollment,preemption
    python scripts/companion_e2e_test.py --port ... --keep-peers   # skip the CRESET
    python scripts/companion_e2e_test.py --port ... --only enrollment,buttonmap,spokenfeeds \
        --articles 5 --render-timeout 10
    python scripts/companion_e2e_test.py --port ... --soak 2       # connection-survival soak

Requires `bleak` and `pyserial` (see scripts/requirements.txt). On macOS the
process running this needs Bluetooth permission — a sandboxed or automated shell
will be refused with BleakBluetoothNotAvailableError, so run it from a terminal
that has been granted access.

Covered:
  enrollment   first contact, on-device confirm, token issued
  reconnect    stored token, silent reconnect, asset digests reported
  buttonmap    ACQUIRE denied with no declaration; accepted after pushing one
  content      atomic title+body+content-id, v11 RENDER_STATUS, held button round trip
  seqgap       v10/v11: a batch that loses a field is discarded whole and answered
  preemption   two sessions on one link, last-requester-wins, in-flight discard
  image        raw packed 2bpp full-screen push, chunk acks, and the decode verdict
  tags         app-declared tags: atomic with content, and state-only writes
  spokenfeeds  mimics a real consumer app: push a batch, then BLOCK for RENDER_STATUS
               before doing anything else, same as SpokenFeeds waiting on it before
               starting audio. A timeout here is "the screen updated but the phone
               heard nothing" -- a distinct failure from a wrong RENDER_STATUS result.
               Self-sufficient: if the session isn't already foreground (e.g. run
               standalone without buttonmap), it pushes a UI declaration and
               ACQUIREs first, so a failure to get the screen fails fast with one
               clear message instead of every push timing out mysteriously.

--soak MINUTES runs a separate mode instead of the scenario groups above: one
connection, held open for the whole window with light periodic activity, to catch
the class of bug where the *link itself* dies rather than any one push failing.
The DLE regression fixed in d1dfd80e killed every connection at exactly ~40s, so
even `--soak 2` is diagnostic.

A note on write types. v9/v10 moved image and title/body CHUNKs to Write
Without Response for throughput, and the sequence numbers this harness sends
exist because of that. This harness splits the difference: title/body go over
Write Without Response, the image does not.

That is not fence-sitting, it is what the device's own 3 s batch-commit
timeout forces. Measured here, a title+body+content-id batch pushed entirely
with response took 11 s on the wire — the device gave up waiting for the
final-flagged field, applied what it had, and logged
"content batch commit flag missed after 3000 ms". Write Without Response is
what makes a text batch fit inside its own commit window, which is precisely
the problem v10 existed to fix. The image stays on Write because bleak has no
cross-platform equivalent of CoreBluetooth's canSendWriteWithoutResponse flow
control: a couple of hundred unthrottled WWR writes would drop some of their
own chunks and turn the image scenario into a coin flip, while a handful for
text does not. The device parses the sequence number regardless of which ATT
write type carried the CHUNK (the protocol calls WWR "recommended, not
enforced at the GATT level"), so the image's wire format is exercised either
way — and both sequence-gap paths are provoked deliberately below rather than
being waited for.
"""

from __future__ import annotations

import argparse
import asyncio
import queue
import random
import sys
import threading
import time
import uuid

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

from bleak import BleakClient, BleakScanner

from companion_protocol import (
    BTN_BACK,
    BTN_CONFIRM,
    BTN_LEFT,
    BTN_RIGHT,
    CAPABILITY_CHAR_UUID,
    FIELD_BODY,
    FIELD_CONTENT_ID,
    FIELD_IMAGE,
    FIELD_TAG_STATE,
    FIELD_TITLE,
    FIELD_UI_DECL,
    PROTOCOL_VERSION,
    RENDER_DISPLAYED,
    RENDER_SEQUENCE_GAP,
    RENDER_RESULTS,
    ROUTING_PAGE_NEXT,
    ROUTING_PAGE_PREV,
    ROUTING_REMOTE,
    SERVICE_UUID,
    SESS_RENDER_STATUS,
    Link,
    Session,
    encode_tag_state,
    encode_ui_declaration,
    pack_2bpp,
    parse_capabilities,
    raw_image_length,
)

# Two distinct simulated apps, so the preemption test exercises the case that
# actually motivated v6: two apps sharing one phone's single BLE link.
APP_A = uuid.UUID("2f1d7b64-9c3e-4a55-8f21-0c7b5e9a3d10").bytes
APP_B = uuid.UUID("7ac41e08-5d62-4f1b-9e33-1b8c4d2f60a5").bytes

DEFAULT_TAGS = [(0, "Saved"), (1, "New")]

DEFAULT_MAP = [
    (BTN_LEFT, ROUTING_PAGE_PREV, "<"),
    (BTN_RIGHT, ROUTING_PAGE_NEXT, ">"),
    (BTN_CONFIRM, ROUTING_REMOTE, "Save"),
    (BTN_BACK, ROUTING_REMOTE, "Back"),
]


# --------------------------------------------------------------------------- #
# Serial side: the device's test console
# --------------------------------------------------------------------------- #


class Console:
    """Thin wrapper over the CMD:/CT: serial protocol in docs/companion-test-console.md.

    A background thread continuously drains the serial port for the whole
    session, not just while a send() is in flight: `[ERR]` log lines (see
    lib/Logging/Logging.h) can be emitted at any point, including during a
    multi-second BLE-only wait like an image push's render+settle, when
    nothing would otherwise be reading the port. `CT:` reply lines go to a
    queue for send() to consume; `[ERR]` lines accumulate for pop_errors().
    Everything else (INF/DBG lines) is dropped, same as before.
    """

    def __init__(self, port: str, baud: int = 115200):
        self.serial = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.3)
        self.serial.reset_input_buffer()
        self._ct_queue: queue.Queue[str] = queue.Queue()
        self._errors: list[str] = []
        self._errors_lock = threading.Lock()
        self._reader_paused = threading.Event()
        self._stop = threading.Event()
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            if self._reader_paused.is_set():
                time.sleep(0.02)
                continue
            raw = self.serial.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            if "CT:" in line:
                self._ct_queue.put(line[line.index("CT:") + 3 :])
            elif "[ERR]" in line:
                with self._errors_lock:
                    self._errors.append(line)

    def close(self) -> None:
        self._stop.set()
        self._reader_thread.join(timeout=1.0)
        self.serial.close()

    def pop_errors(self) -> list[str]:
        """Returns and clears any `[ERR]` log lines captured since the last call."""
        with self._errors_lock:
            errors, self._errors = self._errors, []
        return errors

    def send(self, command: str, expect: str | None = None, timeout: float = 5.0) -> list[str]:
        """Sends one command and collects `CT:` replies until the stream goes quiet."""
        while not self._ct_queue.empty():
            try:
                self._ct_queue.get_nowait()
            except queue.Empty:
                break
        self.serial.write(f"CMD:{command}\n".encode())
        self.serial.flush()

        replies: list[str] = []
        deadline = time.time() + timeout
        last_line = time.time()
        while time.time() < deadline:
            try:
                reply = self._ct_queue.get(timeout=0.1)
            except queue.Empty:
                # Replies arrive together; a quiet gap after at least one means done.
                if replies and time.time() - last_line > 0.4:
                    break
                continue
            replies.append(reply)
            last_line = time.time()
            if expect and reply.startswith(expect):
                deadline = min(deadline, time.time() + 0.4)
        return replies

    def ping(self) -> bool:
        return any(r.startswith("pong") for r in self.send("CPING", expect="pong"))

    def await_screen(self, expected: str, timeout: float = 8.0) -> str:
        """Waits for the device to report `expected`, returning whatever it last said.

        The activity sets its screen under a RenderLock that an in-flight e-ink
        refresh can hold for the best part of a second, so sampling once right
        after a notification races the panel rather than testing anything.
        """
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            last = self.state().get("screen", "")
            if last == expected:
                return last
            time.sleep(0.3)
        return last

    def state(self) -> dict:
        for reply in self.send("CSTATE", expect="state"):
            if reply.startswith("state "):
                out = {}
                for token in reply[6:].split(" "):
                    if "=" in token:
                        key, value = token.split("=", 1)
                        out[key] = value
                return out
        return {}

    def peers(self) -> list[dict]:
        peers = []
        for reply in self.send("CPEERS"):
            if reply.startswith("peer "):
                entry = {}
                for token in reply[5:].split(" "):
                    if "=" in token:
                        key, value = token.split("=", 1)
                        entry[key] = value
                peers.append(entry)
        return peers

    def await_peers(self, minimum: int = 1, timeout: float = 12.0) -> list[dict]:
        """Polls CPEERS until at least `minimum` peers are reported.

        Same reason await_screen() polls: the console answers on the device's
        main loop, which an in-flight full-screen e-ink refresh (exactly what
        confirming a pairing prompt kicks off) can hold for several seconds.
        A single CPEERS right after HELLO_OK sometimes times out waiting for a
        reply that was only ever late, which reads as "the peer was never
        enrolled".
        """
        deadline = time.time() + timeout
        peers: list[dict] = []
        while time.time() < deadline:
            peers = self.peers()
            if len(peers) >= minimum:
                return peers
            time.sleep(0.5)
        return peers

    def tags(self) -> dict:
        """Live tag state, keyed by id. The only way to confirm a write-without-response landed."""
        out = {}
        for reply in self.send("CTAGS"):
            if reply.startswith("tag "):
                fields = {}
                for token in reply[4:].split(" "):
                    if "=" in token:
                        key, value = token.split("=", 1)
                        fields[key] = value
                if "id" in fields:
                    out[int(fields["id"])] = fields.get("state")
        return out

    def press(self, button: int, hold_ms: int = 0) -> None:
        self.send(f"CBTN {button} {hold_ms}", expect="btn")
        # Let the device's loop pick the injection up and act on it.
        time.sleep(0.35 + hold_ms / 1000.0)

    def reset_peers(self) -> None:
        self.send("CRESET", expect="reset")
        time.sleep(0.5)

    def screenshot(self, timeout: float = 35.0, attempts: int = 3) -> bytes:
        """CMD:SCREENSHOT, retried if the dump came back polluted.

        The dump is not exclusive against the device's own logging: anything
        that logs while main.cpp is streaming the framebuffer (a BLE task, the
        render path) writes into the same serial stream and lands *inside* the
        payload, shifting every byte after it. That is detectable — the bytes
        between the header and the footer no longer match the declared size —
        but not repairable from this side, so retry and hope for a quiet
        moment. If every attempt is polluted, the last error stands and the
        caller reports a failure: a silently-shifted framebuffer would fail the
        pixel diff anyway, with a far more confusing message.
        """
        last_error: Exception | None = None
        for attempt in range(attempts):
            try:
                return self._screenshot_once(timeout)
            except ValueError as exc:
                last_error = exc
                time.sleep(1.0)
        raise last_error  # type: ignore[misc]

    def _screenshot_once(self, timeout: float = 35.0) -> bytes:
        """Issues CMD:SCREENSHOT and returns the raw framebuffer bytes.

        The device's response isn't a CT:-prefixed line like every other
        command -- it's a binary dump framed as `SCREENSHOT_START:<size>\\n`,
        then exactly `<size>` raw bytes, then `SCREENSHOT_END\\n` (see the
        SCREENSHOT handler in src/main.cpp). send()'s reader assumes one line
        per reply and filters for "CT:", so it can't carry this; this method
        parses the wire directly instead.

        The 35s budget is deliberately a bit more than the firmware's own 30s
        send-side timeout (main.cpp retries in 256-byte chunks and gives up
        after 30s if the host doesn't drain fast enough) -- that way a
        truncated dump is caught here as a short read/missing footer rather
        than this method timing out first and masking the firmware-side cause.

        Pauses the background reader thread for the duration: it and this
        method's raw reads would otherwise race for the same bytes on the
        wire. The 0.25s pause-settle wait is comfortably longer than the
        reader thread's own 0.2s readline() timeout, so it's guaranteed to
        have noticed the pause and stopped touching the port before this
        method starts reading it directly.
        """
        self._reader_paused.set()
        try:
            time.sleep(0.25)
            self.serial.reset_input_buffer()
            self.serial.write(b"CMD:SCREENSHOT\n")
            self.serial.flush()

            deadline = time.time() + timeout

            header_line = ""
            while time.time() < deadline:
                raw = self.serial.readline()
                if raw:
                    header_line = raw.decode("ascii", "replace").strip()
                    if header_line:
                        break
            if not header_line.startswith("SCREENSHOT_START:"):
                raise ValueError(f"expected SCREENSHOT_START:<size>, got {header_line!r}")
            size = int(header_line.split(":", 1)[1])

            # Read to the footer rather than to a byte count. The count is what
            # the device *meant* to send; the footer is the only thing that
            # says it finished, and the difference between the two is exactly
            # how a log line that interleaved into the dump shows up.
            marker = b"SCREENSHOT_END"
            data = bytearray()
            while time.time() < deadline:
                chunk = self.serial.read(4096)
                if chunk:
                    data.extend(chunk)
                    if marker in data:
                        break
            if marker not in data:
                raise TimeoutError(
                    f"screenshot truncated: {len(data)} bytes and no SCREENSHOT_END before timeout "
                    f"(device declared {size})"
                )

            # Searched for, not required to be the last thing on the wire: the
            # device keeps logging, so a DBG line lands after the footer as
            # readily as inside the dump.
            payload = bytes(data[: data.index(marker)])
            if len(payload) != size:
                raise ValueError(
                    f"screenshot polluted: {len(payload)} bytes between header and footer, device "
                    f"declared {size} — something logged into the serial stream mid-dump"
                )
            return payload
        finally:
            self._reader_paused.clear()


# --------------------------------------------------------------------------- #
# Raw packed 2bpp encoder for field 0x04 (protocol v7+)
# --------------------------------------------------------------------------- #


def make_test_raw_image(width: int, height: int) -> bytes:
    """A raw packed 2bpp ordered-dither gradient, in field 0x04's wire format.

    The packing itself is companion_protocol.pack_2bpp() -- no header, no
    compression, bytesPerRow = ceil(width/4), 4 samples per byte, MSB-first.
    Value 0..3 is the final display level directly (0 black, 3 white) with no
    further gray-level math on the device side. See
    docs/companion-display-protocol.md "Image field (0x04)" for the spec.
    """
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    levels = bytearray(width * height)
    for y in range(height):
        row = bayer[y % 4]
        base = y * width
        for x in range(width):
            intensity = (x * 255) // max(1, width - 1)
            threshold = (row[x % 4] * 255) // 16
            levels[base + x] = min(3, (intensity * 4 + threshold // 4) // 256)
    return pack_2bpp(levels, width, height)


# --------------------------------------------------------------------------- #
# BW-plane ground truth: what CMD:SCREENSHOT should read back after the raw
# 2bpp bytes above are pushed and rendered.
#
# CMD:SCREENSHOT dumps display.getFrameBuffer(), which after a normal
# displayBuffer() push only ever holds the 1-bit black/white plane -- the two
# intermediate gray levels are written straight to the physical panel in a
# transient GRAYSCALE_MSB/LSB pass (see DirectPixelWriter::writePixel()) and
# never land in a readable buffer. So an exact 4-level match isn't possible
# via this mechanism: this only checks the BW plane, i.e. that levels 0-2
# rendered black and level 3 rendered white.
#
# Framebuffer layout (DirectPixelWriter::writePixel(), lib/Epub/Epub/
# converters/DirectPixelWriter.h): 1 bit/pixel, MSB-first,
# byteIndex = phyY * displayWidthBytes + (phyX >> 3), state=true (BW mode)
# clears the bit to draw black; undrawn pixels are left as whatever
# clearScreen() set (0xFF, i.e. white).
#
# Orientation (GfxRenderer::Portrait, the default -- nothing in
# CompanionModeActivity or its setup changes it): phyX = y, phyY =
# (physicalHeight - 1) - x, where physicalHeight is the *logical* width
# reported by the capability characteristic (Portrait swaps physical
# width/height into logical height/width -- see GfxRenderer::getScreenWidth/
# Height()). physicalWidth is therefore the logical height, and the
# framebuffer stride is ceil(physicalWidth / 8) = ceil(logical_height / 8).
# --------------------------------------------------------------------------- #


def compute_expected_bw_framebuffer(raw2bpp: bytes, width: int, height: int) -> bytes:
    """Expected readable-framebuffer bytes for a raw2bpp image pushed at width x height (logical).

    width/height are the logical dimensions from the capability characteristic
    (caps["px_wide"]/["px_high"]), matching what make_test_raw_image() encoded
    and what RawBitmapToFramebufferConverter reads via
    renderer.getScreenWidth()/getScreenHeight(). Thresholds each 2-bit sample
    the same way DirectPixelWriter::writePixel() does in BW render mode:
    sample < 3 -> black (bit cleared), sample == 3 -> left white (bit set).
    """
    src_row_bytes = (width + 3) // 4
    # Portrait: phyH = width (logical width becomes the physical row count),
    # phyW = height (logical height becomes the physical column count).
    stride = (height + 7) // 8
    phys_rows = width
    fb = bytearray(b"\xff" * (stride * phys_rows))

    for y in range(height):
        phy_x = y  # Portrait: phyX = y
        byte_col = phy_x >> 3
        bit_mask = 1 << (7 - (phy_x & 7))
        src_row = memoryview(raw2bpp)[y * src_row_bytes : (y + 1) * src_row_bytes]
        for x in range(width):
            phy_y = (width - 1) - x  # Portrait: phyY = (phyH - 1) - x
            sample = (src_row[x >> 2] >> (6 - (x % 4) * 2)) & 0x03
            if sample < 3:
                idx = phy_y * stride + byte_col
                fb[idx] &= ~bit_mask & 0xFF

    return bytes(fb)


def diff_bw_framebuffer(expected: bytes, actual: bytes, stride: int) -> str | None:
    """Returns None on an exact match, else a short debugging summary.

    Row/col in the summary are physical framebuffer coordinates (post-
    orientation-transform), not the logical image coordinates the source
    bytes were encoded in -- that's what CMD:SCREENSHOT's layout is in.
    """
    if len(expected) != len(actual):
        return f"size mismatch: expected {len(expected)} bytes, got {len(actual)}"
    if expected == actual:
        return None

    n_diff = 0
    first_row = first_col = first_expected = first_actual = None
    for i, (e, a) in enumerate(zip(expected, actual)):
        if e == a:
            continue
        n_diff += 1
        if first_row is None:
            diff_bits = e ^ a
            bit_offset = next(b for b in range(8) if diff_bits & (1 << (7 - b)))
            first_row = i // stride
            first_col = (i % stride) * 8 + bit_offset
            first_expected, first_actual = e, a

    pct = 100.0 * n_diff / len(expected)
    return (
        f"{n_diff} of {len(expected)} bytes differ ({pct:.2f}%); "
        f"first mismatch at physical row {first_row}, pixel-col {first_col} "
        f"(byte expected 0x{first_expected:02x}, got 0x{first_actual:02x})"
    )


def verify_screenshot_matches(raw2bpp: bytes, width: int, height: int, actual_framebuffer: bytes) -> str | None:
    """None if the screenshot's BW plane matches the pushed image, else a diff summary."""
    expected = compute_expected_bw_framebuffer(raw2bpp, width, height)
    stride = (height + 7) // 8
    return diff_bw_framebuffer(expected, actual_framebuffer, stride)


# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #

# [ERR]-level log lines (lib/Logging/Logging.h's LOG_ERR) that are expected
# during a normal run of *this* harness against *this* host. Keep this list
# short and justified: its whole purpose is that everything not on it fails the
# run. A scenario that deliberately drives the device into an error path passes
# its own substrings to check_no_errors(allowed=...) instead of widening this,
# so that exemption stays scoped to the one section that earned it.
ALLOWED_ERR_SUBSTRINGS: tuple[str, ...] = (
    # The peripheral asks for a connection interval and the central declines to
    # honour it for 3 s. This reports the *central's* decision, not a device-side
    # failure: nothing on the device fell back and nothing was dropped, and the
    # firmware keeps working at whatever the link actually settled on.
    #
    # Measured on this host (bleak/CoreBluetooth, 2026-08-03): a connection opens
    # at the central's own 30 ms interval / 720 ms supervision timeout and the
    # peripheral's 15 ms "busy" request is simply never granted, while the later
    # "deep" request is granted exactly. So this can legitimately fire here.
    # It is exempt because it says something true about the host's radio policy
    # rather than about the firmware under test -- but if it starts appearing on
    # a real iPhone it is worth investigating, since the busy profile is what
    # bulk image throughput depends on.
    "conn params NOT honoured after",
    # The harness's own clean disconnect between the first and second BLE
    # links. Deliberately matched with the trailing field-in-flight=0x00: a
    # disconnect that interrupted a transfer reports a non-zero field id there
    # and is *not* exempt, since that one is worth failing on.
    "remote user terminated), field-in-flight=0x00",
)


def check_no_errors(console: "Console", results: "Results", section: str, allowed: tuple[str, ...] = ()) -> None:
    """Fails if the device logged an unexpected [ERR] line during `section`.

    Self-reported status codes (RENDER_STATUS, screen state, etc.) only prove
    the device *thinks* it succeeded. A failure path that logs an error and
    then silently falls back -- e.g. the grayscale settle's storeBwBuffer()
    OOM fallback, which leaves the prior BW frame on screen and returns
    without ever setting an error status -- is invisible to every other check
    in this script. This is the check that catches that shape of bug.
    """
    permitted = ALLOWED_ERR_SUBSTRINGS + allowed
    unexpected = [e for e in console.pop_errors() if not any(a in e for a in permitted)]
    results.check(f"no unexpected [ERR] log lines during [{section}]", not unexpected, "; ".join(unexpected))


class Results:
    def __init__(self):
        self.passed: list[str] = []
        self.failed: list[tuple[str, str]] = []
        self.skipped: list[str] = []

    def check(self, name: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.passed.append(name)
            print(f"  PASS  {name}")
        else:
            self.failed.append((name, detail))
            print(f"  FAIL  {name}{(' — ' + detail) if detail else ''}")
        return condition

    def report(self) -> int:
        print(f"\n{len(self.passed)} passed, {len(self.failed)} failed, {len(self.skipped)} skipped")
        for name, detail in self.failed:
            print(f"  FAILED: {name}{(' — ' + detail) if detail else ''}")
        return 1 if self.failed else 0


# --------------------------------------------------------------------------- #
# SpokenFeeds-behaviour payload generation
#
# Not lorem ipsum -- word-boundary text of a realistic *shape*, because the
# point of this scenario is timing a batch that looks like an actual article,
# not the smallest or largest thing the wire format tolerates.
# --------------------------------------------------------------------------- #

_WORDS = (
    "the quick brown fox jumps over a lazy dog while the news reader waits "
    "for its next article to arrive over bluetooth and render on the panel"
).split()


def _word_salad(rng: random.Random, min_len: int, max_len: int) -> bytes:
    target = rng.randint(min_len, max_len)
    words: list[str] = []
    length = 0
    while length < target:
        word = rng.choice(_WORDS)
        words.append(word)
        length += len(word) + 1
    text = " ".join(words)
    return text[:target].encode("utf-8")


async def push_field_timed(session: Session, field_id: int, data: bytes, final: bool = False,
                            push_id: int = 0) -> float:
    """push_field(), printing the same shape of line CompanionBle.cpp logs for a text field.

    See `LOG_DBG("CBLE", "text field 0x%02x: %u bytes in %u ms%s", ...)` in
    src/CompanionBle.cpp -- this is the wire-side half of that same number, so
    a firmware serial log and this harness's stdout can be diffed side by side.
    """
    start = time.perf_counter()
    await session.push_field(field_id, data, final=final, push_id=push_id)
    elapsed_ms = (time.perf_counter() - start) * 1000
    print(f"  text field {field_id:#04x}: {len(data)} bytes in {elapsed_ms:.0f} ms")
    return elapsed_ms


async def no_render_status_arrives(link: Link, marker: int, window: float) -> list:
    """Waits `window` seconds and returns any RENDER_STATUS seen since `marker`.

    `marker` is a len(link.notifications) snapshot taken before the push. The
    window has to outlast the panel's own settle (~2.2s measured) or "no answer
    arrived" would just mean "the answer had not arrived yet".
    """
    await asyncio.sleep(window)
    return [n for n in link.notifications[marker:] if n.opcode == SESS_RENDER_STATUS]


async def run_tests(args, console: Console, results: Results) -> None:
    selected = set(args.only.split(",")) if args.only else None

    def enabled(name: str) -> bool:
        if selected and name not in selected:
            results.skipped.append(name)
            return False
        return True

    print("Scanning for the device...")
    devices = await BleakScanner.discover(timeout=8.0, service_uuids=[SERVICE_UUID])
    if not devices:
        sys.exit("No companion device advertising. Is it awake and not already connected?")
    device = devices[0]
    print(f"Found {device.name} ({device.address})")

    # Two long-lived identities. A Session outlives any one connection — the
    # reconnect scenario re-attaches session_a to a fresh link with the token
    # it was issued, which is exactly what a phone app does after a dropout.
    session_a = Session(APP_A, "Harness A")
    session_b = Session(APP_B, "Harness B")
    # See "A note on write types" in this file's docstring: text over WWR so a
    # batch fits the device's 3 s commit window, the image left on Write so a
    # couple of hundred unflow-controlled writes can't drop their own chunks.
    session_a.wwr_fields = session_b.wwr_fields = (FIELD_TITLE, FIELD_BODY)

    async with BleakClient(device.address) as client:
        caps = parse_capabilities(
            bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)),
            minimum_version=PROTOCOL_VERSION,
            who="this harness",
        )
        if caps["version"] > PROTOCOL_VERSION:
            # Loud, but not fatal: a newer device is worth running against to
            # see what still holds, and refusing outright is precisely how this
            # harness went five versions without being run.
            print(
                f"WARNING: device speaks v{caps['version']}, this harness was written for "
                f"v{PROTOCOL_VERSION}. Assertions below may test the wrong shape — read the "
                "version history in docs/companion-display-protocol.md and update this file."
            )
        link = Link(client, caps)
        await link.start_notify()
        link.attach(session_a)
        link.attach(session_b)
        print(f"  ATT MTU {client.mtu_size}, chunk payload {link.chunk_payload_size(FIELD_BODY)}B (seq-checked field)")

        # The capability block must match what the device reports over serial —
        # if those two disagree, one of the paths is lying.
        console_cap = next((r for r in console.send("CCAP", expect="cap") if r.startswith("cap ")), "")
        results.check(
            "capability block matches over BLE and serial",
            caps["raw"].hex() in console_cap,
            f"serial said {console_cap!r}",
        )

        # Second drain of CRESET's fallout (main() does the first). The device
        # only discovers its staged image is gone when it next re-renders,
        # which in practice is when a central connects — i.e. right here,
        # several seconds after the reset that caused it. Everything before the
        # first HELLO is pre-run state; the run starts below.
        console.pop_errors()

        # --- enrollment ---------------------------------------------------- #
        if enabled("enrollment"):
            print("\n[enrollment] first contact with an on-device confirm")
            await session_a.send_hello()
            got_pending = False
            try:
                got_pending = await asyncio.wait_for(
                    asyncio.shield(session_a.pending_future()), timeout=5.0
                )
            except asyncio.TimeoutError:
                pass
            results.check("unknown peer raises HELLO_PENDING", bool(got_pending))
            seen = console.await_screen("pairing")
            results.check("device shows the pairing prompt", seen == "pairing",
                          f"screen was {seen!r} — if this never reaches 'pairing', a real user "
                          "has no idea what they are confirming")

            console.press(BTN_CONFIRM)  # the press a BLE-only script cannot make
            reply = await session_a.wait_hello(timeout=10.0)
            results.check("confirm yields HELLO_OK", reply.ok, "" if reply.ok else reply.reason_text)
            if reply.ok:
                results.check("a session id was assigned", session_a.session_id != 0)
                results.check("a 16-byte token was issued", len(session_a.token) == 16)
                results.check(
                    "asset digests report nothing stored",
                    session_a.asset_tags.get(FIELD_UI_DECL) == b"\x00\x00\x00\x00",
                    str(session_a.asset_tags),
                )
                peers = console.await_peers(1)
                results.check("peer appears in the device's index", len(peers) >= 1, str(peers))
            check_no_errors(console, results, "enrollment")

        # --- ACQUIRE gating ------------------------------------------------ #
        if enabled("buttonmap") and session_a.session_id:
            print("\n[buttonmap] ACQUIRE is refused until a button map is stored")
            outcome = await session_a.acquire()
            results.check(
                "ACQUIRE denied with NO_UI_DECLARATION",
                outcome == ("denied", 0),
                f"got {outcome}",
            )

            # Pushed while this session is deliberately NOT foreground — the
            # only order enrollment allows, since ACQUIRE is refused until the
            # declaration exists. A build that requires foreground for asset
            # pushes deadlocks here and this times out.
            button_map = encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS)
            result, tag = await session_a.push_asset(FIELD_UI_DECL, button_map)
            results.check("UI declaration accepted without holding the screen",
                          result == 0, f"result {result}")
            results.check("stored tag is the one pushed", tag == button_map[:4])

            outcome = await session_a.acquire()
            results.check("ACQUIRE now granted", outcome[0] == "foreground", f"got {outcome}")
            results.check("device reports the foreground peer", console.state().get("foreground") != "0")

            labels = console.send("CUI")
            results.check(
                "device read back the declared labels",
                any("label=Save" in line for line in labels),
                str(labels),
            )
            results.check(
                "device read back the declared tags",
                any("tag id=0" in line and "Saved" in line for line in labels),
                str(labels),
            )
            check_no_errors(console, results, "buttonmap")

        # --- content + button round trip ------------------------------------ #
        if enabled("content") and session_a.session_id:
            print("\n[content] atomic push, v11 render status, then a held button round trip")
            body = "\n".join(f"Line {i} of the harness body text." for i in range(60))
            # v11: every field of the batch carries the same pushId, but only
            # the final-flagged one's is retained and echoed. 0x2A is arbitrary
            # and non-zero — 0 would tell the device not to answer at all.
            push_id = 0x2A
            render = session_a.expect_render(push_id)
            started = time.time()
            await session_a.push_field(FIELD_TITLE, b"Harness Title", push_id=push_id)
            await session_a.push_field(FIELD_BODY, body.encode(), push_id=push_id)
            await session_a.push_field(FIELD_CONTENT_ID, b"harness-1", final=True, push_id=push_id)
            wire_done = time.time()
            try:
                result = await asyncio.wait_for(render, timeout=15.0)
            except asyncio.TimeoutError:
                results.check(
                    "a content batch is answered with RENDER_STATUS(Displayed, pushId)", False,
                    f"no RENDER_STATUS for pushId {push_id:#04x} within 15s; "
                    f"saw {session_a.render_statuses}",
                )
            else:
                results.check(
                    "a content batch is answered with RENDER_STATUS(Displayed, pushId)",
                    result == RENDER_DISPLAYED,
                    f"result {RENDER_RESULTS.get(result, result)}",
                )
                # The answer must come from the panel, not the wire: pre-v11 a
                # client could only guess with a fixed delay, and the whole
                # point of the notification is that those two differ by ~2s.
                settle = time.time() - wire_done
                print(f"  wire {wire_done - started:.2f}s, then {settle:.2f}s to the panel")
                results.check(
                    "the answer arrived after the panel settle, not at END",
                    settle > 0.5,
                    f"RENDER_STATUS came {settle:.2f}s after the last END — suspiciously immediate",
                )
            results.check("device is showing text", console.state().get("screen") == "text")

            # pushId 0 is "I am not awaiting an answer", and the device must
            # stay silent rather than notify into the void. This is the half of
            # v11 that costs nothing to get wrong until an app starts counting
            # notifications.
            print("  pushing a second batch with pushId 0 (no answer wanted)")
            marker = len(link.notifications)
            await session_a.push_field(FIELD_TITLE, b"Harness Title (quiet)", push_id=0)
            await session_a.push_field(FIELD_CONTENT_ID, b"harness-1", final=True, push_id=0)
            stray = await no_render_status_arrives(link, marker, window=6.0)
            results.check("pushId 0 produces no RENDER_STATUS at all", not stray,
                          str([(n.result, n.push_id) for n in stray]))

            link.button_events.clear()
            console.press(BTN_CONFIRM, hold_ms=1200)
            await asyncio.sleep(0.5)
            events = list(link.button_events)
            results.check("a held button produced repeat events", len(events) >= 5, f"{len(events)} events")
            results.check(
                "events carry this session and the pushed content-id",
                all(e.session_id == session_a.session_id and e.content_id == b"harness-1" for e in events),
                str(events[:2]),
            )
            results.check("the last event is marked final", bool(events and events[-1].is_final), str(events[-1:]))
            results.check(
                "hold duration rose across the sequence",
                bool(events) and events[-1].ticks > events[0].ticks,
                str([e.ticks for e in events]),
            )

            # LEFT is mapped to local paging, so it must NOT reach the wire.
            link.button_events.clear()
            console.press(BTN_LEFT)
            await asyncio.sleep(0.5)
            results.check("a locally-routed button sends no BLE event", not link.button_events)

            # A local page turn is not a push, so it must not be answered — the
            # device owes exactly one RENDER_STATUS per push, never a broadcast
            # about what happens to be on screen.
            results.check(
                "a local page turn produced no RENDER_STATUS",
                not [n for n in link.notifications[marker:] if n.opcode == SESS_RENDER_STATUS],
                "a redraw the client did not cause was answered",
            )
            check_no_errors(console, results, "content")

        # --- v10/v11 sequence gap ------------------------------------------- #
        if enabled("seqgap") and session_a.session_id:
            print("\n[seqgap] a batch that loses a field is discarded whole and answered")
            # The panel is holding the previous article. Snapshot it: the only
            # honest test of "discarded, not half-applied" is that not one pixel
            # moved -- the console can report *that* the screen is text, never
            # which text.
            try:
                before = console.screenshot()
            except (TimeoutError, ValueError) as exc:
                results.check("framebuffer screenshot round-tripped over serial", False, str(exc))
                before = None

            push_id = 0x5B
            render = session_a.expect_render(push_id)
            session_a.field_seq_gaps.clear()
            # A deliberate skip in the *title*, the case that was a real bug:
            # committing the survivors would have put this batch's body under
            # the previous batch's headline. The body and content-id are clean,
            # so only the batch-level discard can save it.
            # The gap is at chunk 0 (the field's very first CHUNK carries seq 1,
            # as if seq 0 had been lost on the way). Deliberately not a later
            # index: the negotiated MTU decides how many chunks a title of any
            # given length becomes, and a gap at index 1 silently stops
            # happening at all on a link that fits the whole field in one chunk.
            long_title = ("Gap Test " * 40).encode()
            await session_a.push_field(FIELD_TITLE, long_title, push_id=push_id, seq_gap_at=0)
            await session_a.push_field(FIELD_BODY, b"Body that must never reach the panel.", push_id=push_id)
            await session_a.push_field(FIELD_CONTENT_ID, b"harness-gap", final=True, push_id=push_id)

            try:
                result = await asyncio.wait_for(render, timeout=10.0)
            except asyncio.TimeoutError:
                results.check("a poisoned batch is answered RENDER_STATUS(SequenceGap)", False,
                              f"nothing for pushId {push_id:#04x}; saw {session_a.render_statuses}")
            else:
                results.check(
                    "a poisoned batch is answered RENDER_STATUS(SequenceGap)",
                    result == RENDER_SEQUENCE_GAP,
                    f"result {RENDER_RESULTS.get(result, result)}",
                )
            results.check(
                "FIELD_SEQ_GAP named the field that was actually lost",
                session_a.field_seq_gaps == [FIELD_TITLE],
                str(session_a.field_seq_gaps),
            )

            if before is not None:
                # Well past both the settle and the 3s batch timeout, so a
                # late-applied batch would have shown up by now.
                await asyncio.sleep(4.0)
                try:
                    after = console.screenshot()
                except (TimeoutError, ValueError) as exc:
                    results.check("framebuffer screenshot round-tripped over serial", False, str(exc))
                else:
                    results.check(
                        "the discarded batch left the previous page untouched on the panel",
                        before == after,
                        "the framebuffer changed — the batch was applied, at least in part",
                    )

            # A standalone tag push is not part of any batch and must still
            # apply: the discard rule is about batches, not about the device
            # going deaf after one.
            await session_a.push_field(FIELD_TAG_STATE, encode_tag_state([(1, 2)]), final=True)
            await asyncio.sleep(2.0)
            results.check("a standalone tag push still applies after a discarded batch",
                          console.tags().get(1) == "filled", str(console.tags()))
            await session_a.push_field(FIELD_TAG_STATE, encode_tag_state([(1, 0)]), final=True)
            await asyncio.sleep(1.5)

            # The device logs both halves of this deliberately: the dropped
            # field, and the batch it poisoned. Anything else here is a real
            # bug and still fails.
            check_no_errors(console, results, "seqgap", allowed=(
                "CHUNK sequence gap",
                "content batch discarded",
            ))

        # --- tags ------------------------------------------------------------ #
        if enabled("tags") and session_a.session_id:
            print("\n[tags] declared tags, atomic with content and state-only")
            # State-only write, then a content push that says nothing about tags:
            # the tag must survive, because when it clears is app meaning.
            await session_a.set_tag(0, 2)
            await asyncio.sleep(1.5)
            await session_a.push_field(FIELD_BODY, b"A second body push.", final=True)
            await asyncio.sleep(2.0)
            results.check("device still on text after a tag write plus a push",
                          console.state().get("screen") == "text")
            # The Status write is write-without-response, so BLE cannot confirm
            # it landed. Serial can — this is the assertion the harness exists
            # for.
            tags_seen = console.tags()
            results.check("the tag write actually landed on the device",
                          tags_seen.get(0) == "filled", str(tags_seen))
            results.check("a content push did not clear it",
                          tags_seen.get(0) == "filled",
                          "tags must not auto-clear on a body push")

            # An undeclared id must be ignored rather than create a tag.
            await session_a.set_tag(99, 2)
            await asyncio.sleep(1.0)
            results.check("an undeclared tag id creates nothing",
                          len(console.tags()) == len(DEFAULT_TAGS), str(console.tags()))

            # Atomic: content and tag state in one batch, one redraw.
            await session_a.push_field(FIELD_TITLE, b"Tagged article")
            await session_a.push_field(FIELD_BODY, b"Body for the tagged article.")
            await session_a.push_field(FIELD_TAG_STATE, encode_tag_state([(0, 1), (1, 2)]), final=True)
            await asyncio.sleep(2.5)
            results.check("atomic content + tag push kept the device on text",
                          console.state().get("screen") == "text")
            tags_seen = console.tags()
            results.check("both tag states applied from the atomic batch",
                          tags_seen.get(0) == "outline" and tags_seen.get(1) == "filled",
                          str(tags_seen))

            # There is no read-back for tag rendering by design — it is pure
            # drawing. CMD:SCREENSHOT is the visual check.
            await session_a.push_field(FIELD_TAG_STATE, encode_tag_state([(0, 0), (1, 0)]), final=True)
            await asyncio.sleep(1.5)
            check_no_errors(console, results, "tags")

        # --- spokenfeeds: push-then-block, like the real app ----------------- #
        if enabled("spokenfeeds") and session_a.session_id:
            print(
                f"\n[spokenfeeds] {args.articles} article push(es), mimicking a consumer app that "
                f"pushes a batch and then BLOCKS on RENDER_STATUS before doing anything else "
                f"(render timeout {args.render_timeout}s)"
            )

            # A real app can't assume some earlier session already holds the
            # screen. If buttonmap ran first in this process, session_a is
            # already foreground and this is a no-op; run standalone (e.g.
            # --only enrollment,spokenfeeds) and, without this, every push
            # below times out with no clue why -- RENDER_STATUS is
            # deliberately silent for non-foreground sessions.
            group_ready = True
            if console.state().get("foreground") != str(session_a.session_id):
                print("  [spokenfeeds] session is not foreground -- pushing a UI declaration and acquiring")
                button_map = encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS)
                await session_a.push_asset(FIELD_UI_DECL, button_map)
                outcome = await session_a.acquire()
                group_ready = results.check(
                    "spokenfeeds: session holds the screen (foreground)",
                    outcome[0] == "foreground",
                    f"spokenfeeds needs the screen; ACQUIRE failed: got {outcome}",
                )

            rng = random.Random(0xF3ED)
            commit_to_render_ms: list[float] = []
            for i in range(args.articles if group_ready else 0):
                title = _word_salad(rng, 20, 90)
                body = _word_salad(rng, 200, 600)
                # Distinct, non-zero pushId per run so a late answer from a
                # previous run can never be mistaken for this one's.
                push_id = ((0x40 + i) & 0x7F) or 1

                render = session_a.expect_render(push_id)
                await push_field_timed(session_a, FIELD_TITLE, title, push_id=push_id)
                await push_field_timed(session_a, FIELD_BODY, body, final=True, push_id=push_id)
                commit_time = time.perf_counter()

                try:
                    result = await asyncio.wait_for(render, timeout=args.render_timeout)
                except asyncio.TimeoutError:
                    # Exactly the "screen updated but the phone heard nothing" defect
                    # class -- a distinct failure from a wrong RENDER_STATUS result,
                    # so it gets its own message rather than folding into the check below.
                    results.check(
                        f"spokenfeeds run {i + 1}/{args.articles}: RENDER_STATUS arrived "
                        f"within {args.render_timeout}s",
                        False,
                        f"TIMEOUT waiting for pushId {push_id:#04x} -- no RENDER_STATUS arrived; "
                        f"the panel may have updated with nobody told. Seen so far: "
                        f"{session_a.render_statuses}",
                    )
                    continue

                elapsed_ms = (time.perf_counter() - commit_time) * 1000
                commit_to_render_ms.append(elapsed_ms)
                print(f"  batch commit->RENDER_STATUS: {elapsed_ms:.0f} ms")
                results.check(
                    f"spokenfeeds run {i + 1}/{args.articles}: Displayed with the echoed pushId",
                    result == RENDER_DISPLAYED and (result, push_id) in session_a.render_statuses,
                    f"result {RENDER_RESULTS.get(result, result)}",
                )

            if commit_to_render_ms:
                ordered = sorted(commit_to_render_ms)
                mid = len(ordered) // 2
                median_ms = ordered[mid] if len(ordered) % 2 else (ordered[mid - 1] + ordered[mid]) / 2
                per_run = ", ".join(f"{v:.0f}ms" for v in commit_to_render_ms)
                print(
                    f"  spokenfeeds: {len(commit_to_render_ms)}/{args.articles} answered; "
                    f"per-run [{per_run}], median {median_ms:.0f} ms"
                )
            check_no_errors(console, results, "spokenfeeds")

    # --- reconnect with the stored token ------------------------------------ #
    if enabled("reconnect") and session_a.token:
        print("\n[reconnect] stored token, no prompt")
        await asyncio.sleep(2.0)
        async with BleakClient(device.address) as client:
            link = Link(client, caps)
            await link.start_notify()
            link.attach(session_a)
            link.attach(session_b)

            reply = await asyncio.wait_for(session_a.hello(), timeout=10.0)
            results.check("known peer gets HELLO_OK immediately", reply.ok,
                          "" if reply.ok else reply.reason_text)
            results.check("no pairing prompt on screen", console.state().get("screen") != "pairing")
            if reply.ok:
                results.check(
                    "the stored button-map tag is reported back",
                    session_a.asset_tags.get(FIELD_UI_DECL) == encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS)[:4],
                    str(session_a.asset_tags),
                )
                outcome = await session_a.acquire()
                results.check("ACQUIRE granted with no re-push", outcome[0] == "foreground", str(outcome))
            check_no_errors(console, results, "reconnect")

            # --- preemption between two apps on one link -------------------- #
            if enabled("preemption"):
                print("\n[preemption] two apps, one link, last requester wins")
                await session_b.send_hello()
                try:
                    await asyncio.wait_for(asyncio.shield(session_b.pending_future()), timeout=5.0)
                    console.press(BTN_CONFIRM)
                except asyncio.TimeoutError:
                    pass
                reply_b = await session_b.wait_hello(timeout=15.0)
                results.check("second app enrolled on the same link", reply_b.ok,
                              "" if reply_b.ok else reply_b.reason_text)
                if reply_b.ok:
                    results.check(
                        "the two apps got different session ids",
                        session_b.session_id != session_a.session_id,
                        f"{session_a.session_id} vs {session_b.session_id}",
                    )
                    # The device's own serial-reportable session count settles on its next
                    # main-loop iteration, not the instant the BLE notify is sent -- checking
                    # immediately races that, same class as the render-lock delay await_screen()
                    # already retries for.
                    await asyncio.sleep(0.3)
                    results.check("device reports two live sessions", console.state().get("sessions") == "2")

                    await session_b.push_asset(FIELD_UI_DECL, encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS))

                    background = session_a.expect_background()
                    outcome = await session_b.acquire()
                    results.check("the second app took the screen", outcome[0] == "foreground", str(outcome))
                    try:
                        reason = await asyncio.wait_for(background, timeout=5.0)
                        results.check("the first app was told it was preempted", reason == 0, f"reason {reason}")
                    except asyncio.TimeoutError:
                        results.check("the first app was told it was preempted", False, "no background notification arrived")

                    # A push from the now-background app must not change the screen.
                    await session_b.push_field(FIELD_TITLE, b"App B owns the screen", final=True)
                    await asyncio.sleep(1.5)
                    await session_a.push_field(FIELD_TITLE, b"App A should be ignored", final=True)
                    await asyncio.sleep(1.5)
                    results.check(
                        "the foreground peer is still app B",
                        console.state().get("foreground") == str(session_b.session_id),
                        str(console.state()),
                    )

                    # Re-grant: FOREGROUND must fire again, not just the first
                    # time. If it does not, a preempted-then-restored app never
                    # learns to re-push and the reader stays blank.
                    outcome = await session_a.acquire()
                    results.check("FOREGROUND fires again on a re-grant",
                                  outcome[0] == "foreground", str(outcome))
                    results.check("the screen went back to app A",
                                  console.state().get("foreground") == str(session_a.session_id),
                                  str(console.state()))
                    results.check("tags cleared on the handover",
                                  all(state == "hidden" for state in console.tags().values()),
                                  str(console.tags()))
                check_no_errors(console, results, "preemption")

            # --- image push -------------------------------------------------- #
            if enabled("image"):
                # Content/image pushes are silently dropped from a non-foreground
                # session (CompanionBle.cpp only lets the foreground app push visible
                # content; assets like UI declarations are the exception). The
                # preemption block above ends with foreground handed back to session_a,
                # so picking "session_b if it has a session" here would push from a
                # backgrounded peer -- silently dropped, hanging forever waiting for
                # a RENDER_STATUS that will never come. Use whichever peer the device
                # actually reports as foreground right now.
                current_foreground = console.state().get("foreground")
                if session_b.session_id and str(session_b.session_id) == current_foreground:
                    owner = session_b
                else:
                    owner = session_a
                if owner.session_id:
                    print("\n[image] raw packed 2bpp, full screen")
                    raw = make_test_raw_image(caps["px_wide"], caps["px_high"])
                    print(f"  {len(raw)} bytes of raw 2bpp for {caps['px_wide']}x{caps['px_high']}")
                    results.check(
                        "the encoded print is exactly the byte count the device expects",
                        len(raw) == raw_image_length(caps["px_wide"], caps["px_high"]),
                        f"{len(raw)} != {raw_image_length(caps['px_wide'], caps['px_high'])}",
                    )
                    results.check(
                        "the encoded print fits the device's image cap",
                        len(raw) <= caps["max_image"],
                        f"{len(raw)} > {caps['max_image']}",
                    )
                    owner.chunk_acks.clear()
                    image_push_id = 0x77
                    started = time.time()
                    verdict = await owner.push_image(raw, push_id=image_push_id)
                    results.check("device reported the image displayed", verdict == RENDER_DISPLAYED,
                                  f"RENDER_STATUS {RENDER_RESULTS.get(verdict, verdict)}")
                    results.check(
                        "the image's RENDER_STATUS echoed the pushId it was sent with",
                        (RENDER_DISPLAYED, image_push_id) in owner.render_statuses,
                        str(owner.render_statuses),
                    )
                    # v9: a progress marker roughly every 32 CHUNKs. Diagnostic
                    # only, but its absence during a couple of hundred chunks
                    # means the device never saw them as sequenced chunks at all.
                    results.check("IMAGE_CHUNK_ACK arrived during the transfer",
                                  len(owner.chunk_acks) >= 1, str(owner.chunk_acks))
                    print(f"  transfer + develop took {time.time() - started:.1f}s")
                    results.check("device is showing an image", console.state().get("screen") == "image")

                    # The checks above are the device's own self-report (a
                    # notification and a screen-state string) -- a decode bug
                    # that produced garbage pixels but still flipped those flags
                    # would still pass. Pull the actual framebuffer over
                    # CMD:SCREENSHOT and diff it against ground truth computed
                    # from the exact bytes that were pushed.
                    try:
                        actual_fb = console.screenshot()
                    except (TimeoutError, ValueError) as exc:
                        results.check("framebuffer screenshot round-tripped over serial", False, str(exc))
                    else:
                        diff = verify_screenshot_matches(raw, caps["px_wide"], caps["px_high"], actual_fb)
                        results.check(
                            "the framebuffer's BW plane matches the pushed image "
                            "(levels 0-2 black, level 3 white; the two gray levels "
                            "aren't checked -- see compute_expected_bw_framebuffer)",
                            diff is None,
                            diff or "",
                        )
                    check_no_errors(console, results, "image")

                    # v9: a skipped CHUNK sequence number must be caught rather
                    # than decoded into garbage. max_chunks stops the transfer
                    # right after the gap -- the device has already latched it
                    # and ignores the rest, so sending the other ~200 chunks
                    # would only cost wall-clock time.
                    print("  pushing an image with a deliberately skipped sequence number")
                    gap_push_id = 0x78
                    render = owner.expect_render(gap_push_id)
                    await owner.push_field(
                        FIELD_IMAGE, raw, final=True, push_id=gap_push_id, seq_gap_at=2, max_chunks=6
                    )
                    try:
                        verdict = await asyncio.wait_for(render, timeout=30.0)
                    except asyncio.TimeoutError:
                        results.check("a gapped image push is answered RENDER_STATUS(SequenceGap)", False,
                                      f"nothing for pushId {gap_push_id:#04x}; saw {owner.render_statuses}")
                    else:
                        results.check(
                            "a gapped image push is answered RENDER_STATUS(SequenceGap)",
                            verdict == RENDER_SEQUENCE_GAP,
                            f"result {RENDER_RESULTS.get(verdict, verdict)}",
                        )
                    results.check("the previous screen is retained after a gapped image",
                                  console.state().get("screen") == "image", str(console.state()))
                    check_no_errors(console, results, "image seqgap", allowed=("CHUNK sequence gap",))


# --------------------------------------------------------------------------- #
# --soak: connection-survival, not scenario correctness
#
# The DLE regression fixed in d1dfd80e killed every connection at exactly
# ~40s -- no scenario above would have caught that, because every one of them
# either finishes well inside 40s or (image) is slow for reasons that have
# nothing to do with the link itself dying out from under a healthy transfer.
# This mode exists to answer one question only: does a connection survive
# being held open, with light activity, for as long as a real reading session
# would need it to.
# --------------------------------------------------------------------------- #


async def run_soak(args, console: Console, results: Results) -> None:
    print(f"\n[soak] holding one connection open for {args.soak:.1f} minute(s)")
    print("Scanning for the device...")
    devices = await BleakScanner.discover(timeout=8.0, service_uuids=[SERVICE_UUID])
    if not devices:
        sys.exit("No companion device advertising. Is it awake and not already connected?")
    device = devices[0]
    print(f"Found {device.name} ({device.address})")

    disconnected = asyncio.Event()
    disconnect_at: list[float] = []

    def on_disconnect(_client: BleakClient) -> None:
        disconnect_at.append(time.time())
        disconnected.set()

    session = Session(APP_A, "Soak Harness")
    start = time.time()

    async with BleakClient(device.address, disconnected_callback=on_disconnect) as client:
        caps = parse_capabilities(
            bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)),
            minimum_version=PROTOCOL_VERSION,
            who="the soak harness",
        )
        link = Link(client, caps)
        await link.start_notify()
        link.attach(session)

        # Complete the handshake so a Session actually holds the screen for
        # the duration -- a bare connection with no session is a weaker test,
        # since some of what the DLE bug broke was specific to a link doing
        # session-carrying traffic, not an idle GATT connection.
        await session.send_hello()
        try:
            await asyncio.wait_for(asyncio.shield(session.pending_future()), timeout=5.0)
            console.press(BTN_CONFIRM)
        except asyncio.TimeoutError:
            pass
        reply = await session.wait_hello(timeout=15.0)
        if not results.check("soak: handshake completed (HELLO_OK)", reply.ok,
                              "" if reply.ok else reply.reason_text):
            return

        ui = encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS)
        if session.asset_tags.get(FIELD_UI_DECL) != ui[:4]:
            await session.push_asset(FIELD_UI_DECL, ui)
        outcome = await session.acquire()
        results.check("soak: session holds the screen (foreground)", outcome[0] == "foreground", str(outcome))

        deadline = start + args.soak * 60.0
        next_heartbeat = start + 60.0
        next_activity = start + 60.0
        activity_count = 0

        while time.time() < deadline and not disconnected.is_set():
            now = time.time()
            if now >= next_heartbeat:
                print(f"  [soak] heartbeat: uptime {(now - start) / 60:.1f} min, link alive")
                next_heartbeat += 60.0
            if now >= next_activity:
                # Light periodic activity, not a stress load -- a single small
                # push, same shape as a reader idling between articles rather
                # than one that is actively transferring.
                activity_count += 1
                await session.push_field(
                    FIELD_TITLE, f"soak heartbeat {activity_count}".encode(), final=True
                )
                next_activity += 60.0
            try:
                await asyncio.wait_for(disconnected.wait(), timeout=max(0.1, min(1.0, deadline - time.time())))
            except asyncio.TimeoutError:
                pass

        survived = not disconnected.is_set()
        end_time = disconnect_at[0] if disconnect_at else time.time()
        uptime_s = end_time - start

        if survived:
            results.check(
                f"soak: connection survived the full {args.soak:.1f} minute window", True
            )
            print(f"  [soak] completed: uptime {uptime_s / 60:.1f} min, {activity_count} activity round(s) sent")
        else:
            # bleak has no cross-platform surface for the HCI disconnect reason
            # (disconnected_callback's signature is `(client) -> None`; the
            # underlying NSError/HCI code is not forwarded on any backend as of
            # bleak 0.22). Reporting that honestly beats inventing a reason.
            results.check(
                f"soak: connection survived the full {args.soak:.1f} minute window",
                False,
                f"disconnected after {uptime_s:.1f}s ({uptime_s / 60:.2f} min) of "
                f"{args.soak:.1f} requested -- no HCI disconnect reason is exposed by this "
                "BLE stack; check the device's own serial log for the reason it saw",
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="Serial port of a firmware built with -e test")
    parser.add_argument("--only", default=None, help="Comma-separated subset of test groups to run")
    parser.add_argument("--keep-peers", action="store_true", help="Skip the CRESET that starts from unpaired")
    parser.add_argument(
        "--articles", type=int, default=3,
        help="[spokenfeeds] number of article pushes to run, each timed and awaited (default: 3)",
    )
    parser.add_argument(
        "--render-timeout", type=float, default=10.0,
        help="[spokenfeeds] seconds to block for RENDER_STATUS before failing a push (default: 10)",
    )
    parser.add_argument(
        "--soak", type=float, default=None, metavar="MINUTES",
        help="Run the connection-survival soak instead of the scenario groups: hold one "
        "connection open for MINUTES with light periodic activity and assert it survives",
    )
    args = parser.parse_args()

    console = Console(args.port)
    if not console.ping():
        console.close()
        sys.exit(
            "No response to CMD:CPING. Flash a test build first:\n"
            f"  pio run -e test -t upload --upload-port {args.port}"
        )
    print("Serial test console is live.")

    if not args.keep_peers:
        print("Resetting the device to never-paired...")
        console.reset_peers()
        # CRESET deletes the peer directories out from under whatever the
        # activity is currently showing, so a device still displaying the
        # previous run's image logs a failed reload ([RAW2BPP] cannot open
        # .../images/img_N.raw). That is the reset doing its job, not this
        # run's doing, and check_no_errors() would otherwise pin it on the
        # first scenario to look. Anything logged before the run starts is not
        # the run's.
        stale = console.pop_errors()
        if stale:
            print(f"  (discarded {len(stale)} pre-run [ERR] line(s) from the reset)")

    results = Results()
    try:
        if args.soak is not None:
            asyncio.run(run_soak(args, console, results))
        else:
            asyncio.run(run_tests(args, console, results))
    except KeyboardInterrupt:
        pass
    finally:
        console.close()
    sys.exit(results.report())


if __name__ == "__main__":
    main()
