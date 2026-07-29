#!/usr/bin/env python3
"""
End-to-end test harness for Companion Display Protocol v6, on real hardware.

Drives both halves of the device at once: BLE over the host's own radio, and
physical buttons over USB serial. That combination is what makes v6's most
important path testable at all — enrollment needs a CONFIRM press on the device,
which a BLE script cannot produce on its own.

Requires a firmware built with the serial test console:

    pio run -e test -t upload --upload-port /dev/cu.usbmodem21201

Then:

    python scripts/companion_e2e_test.py --port /dev/cu.usbmodem21201
    python scripts/companion_e2e_test.py --port ... --only enrollment,preemption
    python scripts/companion_e2e_test.py --port ... --keep-peers   # skip the CRESET

Requires `bleak` and `pyserial` (see scripts/requirements.txt). On macOS the
process running this needs Bluetooth permission — a sandboxed or automated shell
will be refused with BleakBluetoothNotAvailableError, so run it from a terminal
that has been granted access.

Covered:
  enrollment   first contact, on-device confirm, token issued
  reconnect    stored token, silent reconnect, asset digests reported
  buttonmap    ACQUIRE denied with no declaration; accepted after pushing one
  content      atomic title+body+content-id, then a held button round trip
  preemption   two sessions on one link, last-requester-wins, in-flight discard
  image        raw packed 2bpp full-screen push and the device's decode verdict
  tags         app-declared tags: atomic with content, and state-only writes
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import os
import queue
import struct
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001"
CONTENT_CHAR_UUID = "7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001"
BUTTON_CHAR_UUID = "7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001"
CAPABILITY_CHAR_UUID = "7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001"
STATUS_CHAR_UUID = "7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001"
SESSION_CHAR_UUID = "7c9c0005-3e4a-4b1a-9c1e-6d8a1f2b0001"

OP_START, OP_CHUNK, OP_END = 0x01, 0x02, 0x03
FIELD_TITLE, FIELD_BODY, FIELD_CONTENT_ID = 0x01, 0x02, 0x03
FIELD_IMAGE, FIELD_UI_DECL, FIELD_ICON, FIELD_TAG_STATE = 0x04, 0x05, 0x06, 0x07
FINAL_FLAG = 0x80

SESS_HELLO, SESS_BYE, SESS_ACQUIRE, SESS_RELEASE = 0x01, 0x02, 0x03, 0x04
SESS_HELLO_OK, SESS_HELLO_PENDING, SESS_HELLO_DENIED = 0x81, 0x82, 0x83
SESS_FOREGROUND, SESS_BACKGROUND, SESS_ACQUIRE_DENIED = 0x84, 0x85, 0x86
SESS_ASSET_ACK, SESS_IMAGE_STATUS = 0x87, 0x88

BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN = range(6)
ROUTING_NONE, ROUTING_REMOTE, ROUTING_PAGE_PREV, ROUTING_PAGE_NEXT, ROUTING_SLEEP = range(5)

CHUNK_PAYLOAD = 176

# Two distinct simulated apps, so the preemption test exercises the case that
# actually motivated v6: two apps sharing one phone's single BLE link.
APP_A = uuid.UUID("2f1d7b64-9c3e-4a55-8f21-0c7b5e9a3d10").bytes
APP_B = uuid.UUID("7ac41e08-5d62-4f1b-9e33-1b8c4d2f60a5").bytes


def asset_tag(body: bytes) -> bytes:
    tag = hashlib.sha256(body).digest()[:4]
    return b"\x00\x00\x00\x01" if tag == b"\x00\x00\x00\x00" else tag


def encode_ui_declaration(entries, tags=()) -> bytes:
    body = bytes([len(entries)])
    for button, routing, label in entries:
        encoded = label.encode("utf-8")
        body += bytes([button, routing, len(encoded)]) + encoded
    body += bytes([len(tags)])
    for tag_id, label in tags:
        encoded = label.encode("utf-8")[:12]
        body += bytes([tag_id, len(encoded)]) + encoded
    return asset_tag(body) + body


def encode_tag_state(states) -> bytes:
    out = bytes([len(states)])
    for tag_id, state in states:
        out += bytes([tag_id, state])
    return out


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

    def screenshot(self, timeout: float = 35.0) -> bytes:
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

            data = bytearray()
            while len(data) < size and time.time() < deadline:
                chunk = self.serial.read(size - len(data))
                if chunk:
                    data.extend(chunk)
            if len(data) < size:
                raise TimeoutError(f"screenshot truncated: got {len(data)} of {size} bytes before timeout")

            # Confirm the trailing marker actually arrived rather than assuming
            # the byte count alone means the dump completed cleanly -- a host that
            # falls behind mid-dump is exactly the case the firmware's own 30s
            # send-side timeout guards against, and a truncated/garbled footer is
            # the tell for it.
            footer_line = ""
            while time.time() < deadline:
                raw = self.serial.readline()
                if raw:
                    footer_line = raw.decode("ascii", "replace").strip()
                    if footer_line:
                        break
            if footer_line != "SCREENSHOT_END":
                raise ValueError(f"expected SCREENSHOT_END after {len(data)} bytes, got {footer_line!r}")

            return bytes(data)
        finally:
            self._reader_paused.clear()


# --------------------------------------------------------------------------- #
# BLE side: one simulated app
# --------------------------------------------------------------------------- #


@dataclass
class Peer:
    """One simulated phone app sharing the link."""

    name: str
    app_id: bytes
    install_id: bytes = field(default_factory=lambda: os.urandom(16))
    token: bytes | None = None
    session_id: int = 0
    asset_tags: dict = field(default_factory=dict)


class Link:
    """One BLE connection, carrying one or more simulated apps."""

    def __init__(self, client: BleakClient, caps: dict):
        self.client = client
        self.caps = caps
        self.loop = asyncio.get_running_loop()
        self.pending: dict[str, asyncio.Future] = {}
        self.hello_tags: dict[int, str] = {}
        self.button_events: list[tuple] = []
        self.notifications: list[tuple] = []

    # -- notification routing ---------------------------------------------- #

    def on_session(self, _sender, data: bytearray) -> None:
        opcode = data[0]
        self.notifications.append((opcode, bytes(data)))

        if opcode in (SESS_HELLO_OK, SESS_HELLO_PENDING, SESS_HELLO_DENIED):
            tag = struct.unpack_from("<H", data, 1)[0]
            key = self.hello_tags.get(tag)
            if key is None:
                return  # another app's handshake
            if opcode == SESS_HELLO_PENDING:
                self._resolve(f"pending:{key}", True)
            else:
                self._resolve(f"hello:{key}", bytes(data))
            return

        if opcode == SESS_FOREGROUND:
            self._resolve(f"acquire:{data[1]}", ("foreground", data[1]))
        elif opcode == SESS_ACQUIRE_DENIED:
            self._resolve(f"acquire:{data[1]}", ("denied", data[2]))
        elif opcode == SESS_BACKGROUND:
            self._resolve(f"background:{data[1]}", data[2])
        elif opcode == SESS_ASSET_ACK:
            self._resolve(f"asset:{data[1]}:{data[2]}", (data[3], bytes(data[4:8])))
        elif opcode == SESS_IMAGE_STATUS:
            self._resolve(f"image:{data[1]}", data[2])

    def on_button(self, _sender, data: bytearray) -> None:
        if len(data) < 4:
            return
        self.button_events.append(
            (data[0], data[1] & 0x0F, bool(data[1] & 0x80), struct.unpack_from("<H", data, 2)[0], bytes(data[4:]))
        )

    def _resolve(self, key: str, value) -> None:
        future = self.pending.get(key)
        if future and not future.done():
            self.loop.call_soon_threadsafe(future.set_result, value)

    def expect(self, key: str) -> asyncio.Future:
        future = self.loop.create_future()
        self.pending[key] = future
        return future

    # -- operations --------------------------------------------------------- #

    async def hello(self, peer: Peer, timeout: float = 40.0):
        tag = int.from_bytes(os.urandom(2), "little") or 1
        self.hello_tags[tag] = peer.name
        hello_future = self.expect(f"hello:{peer.name}")
        pending_future = self.expect(f"pending:{peer.name}")

        payload = bytes([SESS_HELLO]) + struct.pack("<H", tag) + peer.app_id + peer.install_id
        payload += bytes([len(peer.token) if peer.token else 0]) + (peer.token or b"")
        encoded = peer.name.encode("utf-8")[:24]
        payload += bytes([len(encoded)]) + encoded
        await self.client.write_gatt_char(SESSION_CHAR_UUID, payload, response=True)
        return hello_future, pending_future

    @staticmethod
    def parse_hello_ok(data: bytes):
        session_id = data[3]
        token = data[4:20]
        tags = {}
        for i in range(data[20]):
            offset = 21 + i * 5
            tags[data[offset]] = data[offset + 1 : offset + 5]
        return session_id, token, tags

    async def acquire(self, peer: Peer, timeout: float = 10.0):
        future = self.expect(f"acquire:{peer.session_id}")
        await self.client.write_gatt_char(
            SESSION_CHAR_UUID, bytes([SESS_ACQUIRE, peer.session_id]), response=True
        )
        return await asyncio.wait_for(future, timeout=timeout)

    async def push_field(self, peer: Peer, field_id: int, data: bytes, final: bool = False) -> None:
        start = bytes([OP_START, field_id | (FINAL_FLAG if final else 0), peer.session_id])
        start += struct.pack("<I", len(data))
        await self.client.write_gatt_char(CONTENT_CHAR_UUID, start, response=True)
        for offset in range(0, len(data), CHUNK_PAYLOAD):
            await self.client.write_gatt_char(
                CONTENT_CHAR_UUID,
                bytes([OP_CHUNK, peer.session_id]) + data[offset : offset + CHUNK_PAYLOAD],
                response=True,
            )
        await self.client.write_gatt_char(CONTENT_CHAR_UUID, bytes([OP_END, peer.session_id]), response=True)

    async def push_asset(self, peer: Peer, field_id: int, payload: bytes, timeout: float = 15.0):
        future = self.expect(f"asset:{peer.session_id}:{field_id}")
        await self.push_field(peer, field_id, payload)
        return await asyncio.wait_for(future, timeout=timeout)

    async def push_image(self, peer: Peer, raw: bytes, timeout: float = 180.0):
        future = self.expect(f"image:{peer.session_id}")
        await self.push_field(peer, FIELD_IMAGE, raw, final=True)
        return await asyncio.wait_for(future, timeout=timeout)

    async def set_tag(self, peer: Peer, tag_id: int, state: int) -> None:
        await self.client.write_gatt_char(
            STATUS_CHAR_UUID, bytes([peer.session_id, tag_id, state]), response=True
        )


# --------------------------------------------------------------------------- #
# Raw packed 2bpp encoder for field 0x04 (protocol v7+)
# --------------------------------------------------------------------------- #


def make_test_raw_image(width: int, height: int) -> bytes:
    """A raw packed 2bpp ordered-dither gradient, in field 0x04's wire format.

    No header, no compression: bytesPerRow = ceil(width/4) bytes, 4 samples per
    byte, MSB-first (sample 0 in bits 7-6), rows top to bottom. Value 0..3 is
    the final display level directly -- 0 black, 3 white -- with no further
    gray-level math on the device side. See docs/companion-display-protocol.md
    "Image field (0x04)" for the authoritative spec.
    """
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    row_bytes = (width + 3) // 4
    out = bytearray(row_bytes * height)
    for y in range(height):
        row = memoryview(out)[y * row_bytes : (y + 1) * row_bytes]
        for x in range(width):
            intensity = (x * 255) // max(1, width - 1)
            threshold = (bayer[y % 4][x % 4] * 255) // 16
            level = min(3, (intensity * 4 + threshold // 4) // 256)
            shift = 6 - (x % 4) * 2
            row[x // 4] |= level << shift
    return bytes(out)


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

# [ERR]-level log lines (lib/Logging/Logging.h's LOG_ERR) expected during
# normal operation, if a scenario is ever added that deliberately drives the
# device into an error path (e.g. exhausting session slots). Empty for now --
# nothing any current scenario does is expected to log an ERR line, so any
# that appear are a real bug, not test noise. Add a substring here (matched
# with `in`, not exact-match) alongside the scenario that expects it, with a
# comment explaining why it's expected.
ALLOWED_ERR_SUBSTRINGS: tuple[str, ...] = ()


def check_no_errors(console: "Console", results: "Results", section: str) -> None:
    """Fails if the device logged an unexpected [ERR] line during `section`.

    Self-reported status codes (IMAGE_STATUS, screen state, etc.) only prove
    the device *thinks* it succeeded. A failure path that logs an error and
    then silently falls back -- e.g. the grayscale settle's storeBwBuffer()
    OOM fallback, which leaves the prior BW frame on screen and returns
    without ever setting an error status -- is invisible to every other check
    in this script. This is the check that catches that shape of bug.
    """
    unexpected = [e for e in console.pop_errors() if not any(a in e for a in ALLOWED_ERR_SUBSTRINGS)]
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


def parse_capabilities(raw: bytes) -> dict:
    if len(raw) < 23:
        sys.exit(f"Capability block is {len(raw)} bytes, expected 23 — is this a v6 build?")
    if raw[0] != 6:
        sys.exit(f"Device speaks protocol v{raw[0]}, this harness speaks v6.")
    return {
        "version": raw[0],
        "max_text": struct.unpack_from("<H", raw, 3)[0],
        "flags": raw[5],
        "max_image": struct.unpack_from("<I", raw, 6)[0],
        "max_sessions": raw[10],
        "icon_w": raw[11],
        "icon_h": raw[12],
        "device_id": raw[13:17].hex(),
        "px_wide": struct.unpack_from("<H", raw, 17)[0],
        "px_high": struct.unpack_from("<H", raw, 19)[0],
        "raw": raw,
    }


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

    async with BleakClient(device.address) as client:
        caps = parse_capabilities(bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)))
        link = Link(client, caps)
        await client.start_notify(SESSION_CHAR_UUID, link.on_session)
        await client.start_notify(BUTTON_CHAR_UUID, link.on_button)

        # The capability block must match what the device reports over serial —
        # if those two disagree, one of the paths is lying.
        console_cap = next((r for r in console.send("CCAP", expect="cap") if r.startswith("cap ")), "")
        results.check(
            "capability block matches over BLE and serial",
            caps["raw"].hex() in console_cap,
            f"serial said {console_cap!r}",
        )

        peer_a = Peer("Harness A", APP_A)
        peer_b = Peer("Harness B", APP_B)

        # --- enrollment ---------------------------------------------------- #
        if enabled("enrollment"):
            print("\n[enrollment] first contact with an on-device confirm")
            hello_future, pending_future = await link.hello(peer_a)
            got_pending = False
            try:
                got_pending = await asyncio.wait_for(pending_future, timeout=5.0)
            except asyncio.TimeoutError:
                pass
            results.check("unknown peer raises HELLO_PENDING", bool(got_pending))
            seen = console.await_screen("pairing")
            results.check("device shows the pairing prompt", seen == "pairing",
                          f"screen was {seen!r} — if this never reaches 'pairing', a real user "
                          "has no idea what they are confirming")

            console.press(BTN_CONFIRM)  # the press a BLE-only script cannot make
            data = await asyncio.wait_for(hello_future, timeout=10.0)
            results.check("confirm yields HELLO_OK", data[0] == SESS_HELLO_OK, f"got {data[0]:#04x}")
            if data[0] == SESS_HELLO_OK:
                peer_a.session_id, peer_a.token, peer_a.asset_tags = link.parse_hello_ok(data)
                results.check("a session id was assigned", peer_a.session_id != 0)
                results.check("a 16-byte token was issued", len(peer_a.token) == 16)
                results.check(
                    "asset digests report nothing stored",
                    peer_a.asset_tags.get(FIELD_UI_DECL) == b"\x00\x00\x00\x00",
                    str(peer_a.asset_tags),
                )
                results.check("peer appears in the device's index", len(console.peers()) >= 1)
            check_no_errors(console, results, "enrollment")

        # --- ACQUIRE gating ------------------------------------------------ #
        if enabled("buttonmap") and peer_a.session_id:
            print("\n[buttonmap] ACQUIRE is refused until a button map is stored")
            outcome = await link.acquire(peer_a)
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
            result, tag = await link.push_asset(peer_a, FIELD_UI_DECL, button_map)
            results.check("UI declaration accepted without holding the screen",
                          result == 0, f"result {result}")
            results.check("stored tag is the one pushed", tag == button_map[:4])

            outcome = await link.acquire(peer_a)
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
        if enabled("content") and peer_a.session_id:
            print("\n[content] atomic push, then a held button round trip")
            body = "\n".join(f"Line {i} of the harness body text." for i in range(60))
            await link.push_field(peer_a, FIELD_TITLE, b"Harness Title")
            await link.push_field(peer_a, FIELD_BODY, body.encode())
            await link.push_field(peer_a, FIELD_CONTENT_ID, b"harness-1", final=True)
            await asyncio.sleep(2.0)
            results.check("device is showing text", console.state().get("screen") == "text")

            link.button_events.clear()
            console.press(BTN_CONFIRM, hold_ms=1200)
            await asyncio.sleep(0.5)
            events = list(link.button_events)
            results.check("a held button produced repeat events", len(events) >= 5, f"{len(events)} events")
            results.check(
                "events carry this session and the pushed content-id",
                all(e[0] == peer_a.session_id and e[4] == b"harness-1" for e in events),
                str(events[:2]),
            )
            results.check("the last event is marked final", bool(events and events[-1][2]), str(events[-1:]))
            results.check(
                "hold duration rose across the sequence",
                bool(events) and events[-1][3] > events[0][3],
                str([e[3] for e in events]),
            )

            # LEFT is mapped to local paging, so it must NOT reach the wire.
            link.button_events.clear()
            console.press(BTN_LEFT)
            await asyncio.sleep(0.5)
            results.check("a locally-routed button sends no BLE event", not link.button_events)
            check_no_errors(console, results, "content")

        # --- tags ------------------------------------------------------------ #
        if enabled("tags") and peer_a.session_id:
            print("\n[tags] declared tags, atomic with content and state-only")
            # State-only write, then a content push that says nothing about tags:
            # the tag must survive, because when it clears is app meaning.
            await link.set_tag(peer_a, 0, 2)
            await asyncio.sleep(1.5)
            await link.push_field(peer_a, FIELD_BODY, b"A second body push.", final=True)
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
            await link.set_tag(peer_a, 99, 2)
            await asyncio.sleep(1.0)
            results.check("an undeclared tag id creates nothing",
                          len(console.tags()) == len(DEFAULT_TAGS), str(console.tags()))

            # Atomic: content and tag state in one batch, one redraw.
            await link.push_field(peer_a, FIELD_TITLE, b"Tagged article")
            await link.push_field(peer_a, FIELD_BODY, b"Body for the tagged article.")
            await link.push_field(peer_a, FIELD_TAG_STATE, encode_tag_state([(0, 1), (1, 2)]), final=True)
            await asyncio.sleep(2.5)
            results.check("atomic content + tag push kept the device on text",
                          console.state().get("screen") == "text")
            tags_seen = console.tags()
            results.check("both tag states applied from the atomic batch",
                          tags_seen.get(0) == "outline" and tags_seen.get(1) == "filled",
                          str(tags_seen))

            # There is no read-back for tag rendering by design — it is pure
            # drawing. CMD:SCREENSHOT is the visual check.
            await link.push_field(peer_a, FIELD_TAG_STATE, encode_tag_state([(0, 0), (1, 0)]), final=True)
            await asyncio.sleep(1.5)
            check_no_errors(console, results, "tags")

        # --- reconnect with the stored token -------------------------------- #
        if enabled("reconnect") and peer_a.token:
            print("\n[reconnect] stored token, no prompt")
            stored_token = peer_a.token

    if enabled("reconnect") and peer_a.token:
        await asyncio.sleep(2.0)
        async with BleakClient(device.address) as client:
            link = Link(client, caps)
            await client.start_notify(SESSION_CHAR_UUID, link.on_session)
            await client.start_notify(BUTTON_CHAR_UUID, link.on_button)

            hello_future, pending_future = await link.hello(peer_a)
            data = await asyncio.wait_for(hello_future, timeout=10.0)
            results.check("known peer gets HELLO_OK immediately", data[0] == SESS_HELLO_OK)
            results.check("no pairing prompt on screen", console.state().get("screen") != "pairing")
            if data[0] == SESS_HELLO_OK:
                peer_a.session_id, _, tags = link.parse_hello_ok(data)
                results.check(
                    "the stored button-map tag is reported back",
                    tags.get(FIELD_UI_DECL) == encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS)[:4],
                    str(tags),
                )
                outcome = await link.acquire(peer_a)
                results.check("ACQUIRE granted with no re-push", outcome[0] == "foreground", str(outcome))
            check_no_errors(console, results, "reconnect")

            # --- preemption between two apps on one link -------------------- #
            if enabled("preemption"):
                print("\n[preemption] two apps, one link, last requester wins")
                hello_future_b, pending_future_b = await link.hello(peer_b)
                try:
                    await asyncio.wait_for(pending_future_b, timeout=5.0)
                    console.press(BTN_CONFIRM)
                except asyncio.TimeoutError:
                    pass
                data_b = await asyncio.wait_for(hello_future_b, timeout=15.0)
                results.check("second app enrolled on the same link", data_b[0] == SESS_HELLO_OK)
                if data_b[0] == SESS_HELLO_OK:
                    peer_b.session_id, peer_b.token, _ = link.parse_hello_ok(data_b)
                    results.check(
                        "the two apps got different session ids",
                        peer_b.session_id != peer_a.session_id,
                        f"{peer_a.session_id} vs {peer_b.session_id}",
                    )
                    # The device's own serial-reportable session count settles on its next
                    # main-loop iteration, not the instant the BLE notify is sent -- checking
                    # immediately races that, same class as the render-lock delay await_screen()
                    # already retries for.
                    await asyncio.sleep(0.3)
                    results.check("device reports two live sessions", console.state().get("sessions") == "2")

                    await link.push_asset(peer_b, FIELD_UI_DECL, encode_ui_declaration(DEFAULT_MAP, DEFAULT_TAGS))

                    background_future = link.expect(f"background:{peer_a.session_id}")
                    outcome = await link.acquire(peer_b)
                    results.check("the second app took the screen", outcome[0] == "foreground", str(outcome))
                    try:
                        reason = await asyncio.wait_for(background_future, timeout=5.0)
                        results.check("the first app was told it was preempted", reason == 0, f"reason {reason}")
                    except asyncio.TimeoutError:
                        results.check("the first app was told it was preempted", False, "no background notification arrived")

                    # A push from the now-background app must not change the screen.
                    await link.push_field(peer_b, FIELD_TITLE, b"App B owns the screen", final=True)
                    await asyncio.sleep(1.5)
                    await link.push_field(peer_a, FIELD_TITLE, b"App A should be ignored", final=True)
                    await asyncio.sleep(1.5)
                    results.check(
                        "the foreground peer is still app B",
                        console.state().get("foreground") == str(peer_b.session_id),
                        str(console.state()),
                    )

                    # Re-grant: FOREGROUND must fire again, not just the first
                    # time. If it does not, a preempted-then-restored app never
                    # learns to re-push and the reader stays blank.
                    outcome = await link.acquire(peer_a)
                    results.check("FOREGROUND fires again on a re-grant",
                                  outcome[0] == "foreground", str(outcome))
                    results.check("the screen went back to app A",
                                  console.state().get("foreground") == str(peer_a.session_id),
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
                # preemption block above ends with foreground handed back to peer_a,
                # so picking "peer_b if it has a session" here would push from a
                # backgrounded peer -- silently dropped, hanging forever waiting for
                # an IMAGE_STATUS that will never come. Use whichever peer the device
                # actually reports as foreground right now.
                current_foreground = console.state().get("foreground")
                if peer_b.session_id and str(peer_b.session_id) == current_foreground:
                    owner = peer_b
                else:
                    owner = peer_a
                if owner.session_id:
                    print("\n[image] raw packed 2bpp, full screen")
                    raw = make_test_raw_image(caps["px_wide"], caps["px_high"])
                    print(f"  {len(raw)} bytes of raw 2bpp for {caps['px_wide']}x{caps['px_high']}")
                    results.check(
                        "the encoded print fits the device's image cap",
                        len(raw) <= caps["max_image"],
                        f"{len(raw)} > {caps['max_image']}",
                    )
                    started = time.time()
                    verdict = await link.push_image(owner, raw)
                    results.check("device reported the image displayed", verdict == 0, f"IMAGE_STATUS {verdict}")
                    print(f"  transfer + develop took {time.time() - started:.1f}s")
                    results.check("device is showing an image", console.state().get("screen") == "image")

                    # The two checks above are the device's own self-report
                    # (an IMAGE_STATUS notification and a screen-state string)
                    # -- a decode bug that produced garbage pixels but still
                    # flipped those flags would still pass both. Pull the
                    # actual framebuffer over CMD:SCREENSHOT and diff it
                    # against ground truth computed from the exact bytes that
                    # were pushed, for real pixel-level verification.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="Serial port of a firmware built with -e test")
    parser.add_argument("--only", default=None, help="Comma-separated subset of test groups to run")
    parser.add_argument("--keep-peers", action="store_true", help="Skip the CRESET that starts from unpaired")
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

    results = Results()
    try:
        asyncio.run(run_tests(args, console, results))
    except KeyboardInterrupt:
        pass
    finally:
        console.close()
    sys.exit(results.report())


if __name__ == "__main__":
    main()
