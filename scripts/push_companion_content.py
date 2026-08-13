#!/usr/bin/env python3
"""
Drive a Companion Mode device over BLE from a dev machine, without a phone app.

Implements the client side of docs/companion-display-protocol.md: the HELLO
handshake, token persistence, the button map, icons, text and image pushes. The
wire format itself lives in scripts/companion_protocol.py, shared with the e2e
harness; this file is only the policy on top of it — which buttons and tags this
"app" declares, what it pushes, and what it prints. This is the fastest way to
exercise the firmware — it does not depend on either iOS app being ready, and it
can produce inputs (a full-panel raw image, a 10-page body, a deliberately
malformed asset) that are awkward to trigger from an app.

Usage:
    python scripts/push_companion_content.py                        # push default text
    python scripts/push_companion_content.py --body-file article.txt
    python scripts/push_companion_content.py --image photo.raw      # pre-packed raw 2bpp
    python scripts/push_companion_content.py --image-from photo.jpg # dither + pack it here first (needs Pillow)
    python scripts/push_companion_content.py --icon icon.png        # 1-bpp sleep-screen icon (needs Pillow)
    python scripts/push_companion_content.py --listen               # stay connected, print button events
    python scripts/push_companion_content.py --forget               # drop the stored token, re-pair
    python scripts/push_companion_content.py --await-render         # block for RENDER_STATUS, print latency

The pairing token is kept in ~/.crosspoint_companion_tokens.json, keyed by the
device id, so the second run onward needs no on-device confirmation.

Since v12 a peer declares one content shape and may push only the fields that
belong to it. This script picks its shape from its arguments — IMAGE for
--image/--image-from, TEXT otherwise — and re-declares when that changes, which
is the protocol's sanctioned (screen-clearing) way to switch. Requires a v12+
device: an older one would misparse the declaration's new leading shape byte.

Requires `bleak` (see scripts/requirements.txt); `Pillow` only for --image-from
and --icon. On macOS, the first run prompts for Bluetooth permission for the
terminal/Python process.
"""

import argparse
import asyncio
import statistics
import sys
import time
import uuid
from pathlib import Path

from bleak import BleakClient, BleakScanner

from companion_protocol import (
    ACQUIRE_DENIED_REASONS,
    ASSET_RESULTS,
    CAPABILITY_CHAR_UUID,
    FIELD_BODY,
    FIELD_CONTENT_ID,
    FIELD_ICON,
    FIELD_IMAGE,
    FIELD_TAG_STATE,
    FIELD_TITLE,
    FIELD_UI_DECL,
    PROTOCOL_VERSION,
    RENDER_DISPLAYED,
    RENDER_RESULTS,
    ROUTING_PAGE_NEXT,
    ROUTING_PAGE_PREV,
    ROUTING_REMOTE,
    SERVICE_UUID,
    SHAPE_IMAGE,
    SHAPE_NAMES,
    SHAPE_TEXT,
    BACKGROUND_REASONS,
    Link,
    Session,
    encode_icon_bits,
    encode_tag_state,
    encode_ui_declaration,
    install_id,
    pack_2bpp,
    parse_capabilities,
    raw_image_length,
    remember_token,
    stored_token,
)

# A stable appId for this script. It is a real app identity as far as the device
# is concerned — its own peer directory, its own icon tile.
SCRIPT_APP_ID = uuid.UUID("2f1d7b64-9c3e-4a55-8f21-0c7b5e9a3d10").bytes
DISPLAY_NAME = "Dev Pusher"

DEFAULT_TITLE = "Test Article: BLE Push"
DEFAULT_BODY = (
    "This is a test article pushed directly over BLE, without the phone "
    "app. Use --title/--body/--body-file to push your own content."
)

# What this script declares about its own UI. LEFT/RIGHT page the buffered body
# on-device; BACK/CONFIRM are forwarded so --listen can print them. The tags
# are this script's own invention — the device defines none. UP/DOWN are left
# unrouted (None) rather than Remote: the firmware claims them for image
# gallery prev/next while an image is on screen (see CompanionModeActivity::
# handleGalleryNav()), and routing them here would block that for this
# script's own pushes.
BUTTON_MAP = [
    (2, ROUTING_PAGE_PREV, "<"),
    (3, ROUTING_PAGE_NEXT, ">"),
    (1, ROUTING_REMOTE, "Save"),
    (0, ROUTING_REMOTE, "Back"),
]

TAGS = [(0, "Saved"), (1, "New")]


def declaration(shape: int) -> bytes:
    """This script's UI declaration for one content shape (v12's mandatory byte).

    The shape is a parameter rather than a constant because this script is the
    one client that legitimately pushes both kinds: `--image` makes it an IMAGE
    peer, everything else a TEXT peer. A peer may only push fields matching the
    shape it declared, so the shape has to be decided from the arguments before
    the declaration goes out.

    Switching between runs is the protocol's sanctioned escape hatch, not a
    loophole: a different shape changes the digest, so the push below fires,
    and re-pushing a declaration triggers a foreground change that clears the
    screen. That is exactly the intended cost of an app re-declaring what it is.
    """
    return encode_ui_declaration(BUTTON_MAP, TAGS, shape=shape)


# --------------------------------------------------------------------------- #
# Version gates
#
# v12 raised the floor for this script to v12 outright. Until then it accepted
# any v6+ device, because it only exercised the stable-since-v6 subset. That
# stopped being true when the content shape became a mandatory byte at the
# *front* of the UI declaration: a v11 device reads that byte as the button
# count and walks the rest of the asset off its own layout. There is no version
# of that failure that is loud, so the gate has to be, and a clean break is
# exactly the case where refusing early beats degrading.
#
# The older gates below still apply for the same reason: two fields moved to
# a sequence-numbered CHUNK (image in v9, title/body in v10) and the
# framer in companion_protocol always sends that sequence number. To a pre-v9/
# pre-v10 device those two bytes are not a header, they are the first two bytes
# of the payload, so the push would not fail, it would silently corrupt. Refuse
# with a real message instead.
# --------------------------------------------------------------------------- #


def require_version(caps: dict, minimum: int, what: str) -> None:
    if caps["version"] < minimum:
        raise SystemExit(f"Device speaks protocol v{caps['version']}, {what} needs v{minimum}+.")


# --------------------------------------------------------------------------- #
# Optional Pillow-backed encoders
# --------------------------------------------------------------------------- #


def dither_to_raw_bitmap(path: str, width: int, height: int) -> bytes:
    """Crop-to-fill, dither to the panel's 4 levels, pack as field 0x04's wire
    format: raw 2-bit samples, no header, bytesPerRow = ceil(width/4), each
    byte MSB-first (bits 7-6 = leftmost sample), rows top-to-bottom.

    Deliberately simple — this is a bring-up tool, not the Polaroid app. The
    real aesthetic decision belongs to the phone app; the only contract this
    has to honour is that every pixel ends up as one of the four levels 0-3
    (0=black..3=white), packed exactly as the device expects.
    """
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("--image-from needs Pillow: pip install Pillow")

    source = Image.open(path).convert("L")
    scale = max(width / source.width, height / source.height)
    resized = source.resize((max(1, round(source.width * scale)), max(1, round(source.height * scale))), Image.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    cropped = resized.crop((left, top, left + width, top + height))

    # Floyd-Steinberg onto a 4-entry palette, then map palette indices to the
    # 2-bit sample values 0-3 the device unpacks directly (no gray-expansion).
    palette = Image.new("P", (1, 1))
    palette.putpalette([0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255] + [0] * (768 - 12))
    quantized = cropped.convert("RGB").quantize(palette=palette, dither=Image.FLOYDSTEINBERG)
    levels = list(quantized.getdata())  # already palette indices 0-3
    return pack_2bpp(levels, width, height)


def encode_icon(path: str, width: int, height: int) -> bytes:
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("--icon needs Pillow: pip install Pillow")

    image = Image.open(path).convert("L").resize((width, height), Image.LANCZOS)
    bits = bytearray((width * height) // 8)
    for y in range(height):
        for x in range(width):
            if image.getpixel((x, y)) < 128:  # dark pixel = ink
                bits[y * (width // 8) + (x // 8)] |= 0x80 >> (x % 8)
    return encode_icon_bits(bytes(bits))


# --------------------------------------------------------------------------- #
# Reporting hooks
#
# companion_protocol never prints — a client decides what is worth saying. These
# are the notifications this script surfaces as they arrive rather than awaiting.
# --------------------------------------------------------------------------- #


def report_pending() -> None:
    print("  Device is asking the user to confirm. Press CONFIRM on the device.")


def report_background(reason: int) -> None:
    print(f"  BACKGROUND ({BACKGROUND_REASONS.get(reason, reason)})")


def report_chunk_ack(seq: int) -> None:
    # Diagnostic only -- progress marker during an in-flight image push, well
    # before the final RENDER_STATUS. Nothing here awaits it.
    print(f"  chunk ack: seq={seq}")


def report_field_seq_gap(field_id: int) -> None:
    # v10: a title/body CHUNK sequence number skipped ahead of what the device
    # expected -- the field was dropped, not rendered, and if it was part of an
    # atomic batch the whole batch went with it. Print-only: this script pushes
    # text with pushId 0 (see push_content()), so there is no render future to
    # fail either.
    print(f"  FIELD_SEQ_GAP: field {field_id:#04x} dropped (CHUNK sequence gap)")


def report_button(event) -> None:
    suffix = f" content-id={event.content_id!r}" if event.content_id else ""
    final = " FINAL" if event.is_final else ""
    print(f"  [session {event.session_id}] {event.name} held {event.seconds:.1f}s{final}{suffix}")


# --------------------------------------------------------------------------- #
# Pushes
# --------------------------------------------------------------------------- #


async def push_asset(session: Session, field_id: int, payload: bytes) -> int:
    result, tag = await session.push_asset(field_id, payload)
    print(f"  asset {field_id:#04x}: {ASSET_RESULTS.get(result, result)} (tag {tag.hex()})")
    return result


async def push_image(session: Session, caps: dict, raw_bitmap: bytes) -> int:
    require_version(caps, 9, "image push")
    expected = raw_image_length(caps["px_wide"], caps["px_high"])
    if len(raw_bitmap) != expected:
        raise SystemExit(
            f"Raw bitmap is {len(raw_bitmap)} bytes, device expects exactly {expected} "
            f"({caps['px_wide']}x{caps['px_high']}, packed 2bpp)."
        )
    if len(raw_bitmap) > caps["max_image"]:
        raise SystemExit(f"Image is {len(raw_bitmap)} bytes, device cap is {caps['max_image']}.")

    def progress(index: int, total: int) -> None:
        if index % 25 == 0 or index == total - 1:
            print(f"\r  sending {index + 1}/{total} chunks", end="", flush=True)

    # push_id=1: this script only ever has one push in flight at a time
    # (everything here is a single sequential `await`, never overlapped), so
    # there is no correlation to get wrong -- any non-zero constant would do. 1
    # just reads as "the id", not as anything meaningful. It has to be non-zero
    # though: pushId 0 tells the device not to answer at all, and the wait
    # below would then burn its full timeout for nothing.
    render = session.expect_render(1)
    transfer_start = time.perf_counter()
    await session.push_field(FIELD_IMAGE, raw_bitmap, final=True, push_id=1, progress=progress)
    print()
    transfer_s = time.perf_counter() - transfer_start
    print(f"  BLE transfer: {transfer_s:.2f}s ({len(raw_bitmap) / transfer_s:.0f} B/s)")
    print("  Waiting for the device to develop it (decode + grayscale settle)...")
    result = await asyncio.wait_for(render, timeout=180)
    print(f"  image: {RENDER_RESULTS.get(result, result)}")
    return result


async def push_content(session: Session, caps: dict, args) -> None:
    require_version(caps, 10, "title/body push")
    body = Path(args.body_file).read_text(encoding="utf-8") if args.body_file else (args.body or DEFAULT_BODY)
    print("Pushing title + body + content-id as one atomic batch...")
    # pushId 0 by default: this script does not normally await a RENDER_STATUS
    # for text, and 0 tells the device not to send one rather than notifying
    # into the void. --await-render opts into a real (non-zero) pushId instead,
    # for exactly the case this script exists to make ad hoc: "did the panel
    # actually settle, and how long did that take."
    push_id = 1 if args.await_render else 0
    render = session.expect_render(push_id) if args.await_render else None
    started = time.perf_counter()
    await session.push_field(FIELD_TITLE, args.title.encode("utf-8"), push_id=push_id)
    await session.push_field(FIELD_BODY, body.encode("utf-8"), push_id=push_id)
    # content-id carries the final flag: the device only commits and redraws
    # once this last field's END arrives, so title+body land together instead
    # of the headline updating first.
    if args.tag is not None:
        # Tags last, carrying the final flag: content and tag state then commit
        # in one redraw rather than the tag flipping separately.
        await session.push_field(FIELD_CONTENT_ID, args.content_id.encode("utf-8")[:32], push_id=push_id)
        await session.push_field(FIELD_TAG_STATE, encode_tag_state([tuple(args.tag)]), final=True, push_id=push_id)
    else:
        await session.push_field(FIELD_CONTENT_ID, args.content_id.encode("utf-8")[:32], final=True, push_id=push_id)
    wire_s = time.perf_counter() - started
    print(f"  Pushed ({wire_s:.2f}s on the wire).")

    if args.await_render:
        print(f"  Waiting up to {args.render_timeout:.0f}s for RENDER_STATUS...")
        try:
            result = await asyncio.wait_for(render, timeout=args.render_timeout)
        except asyncio.TimeoutError:
            raise SystemExit(
                f"TIMEOUT: no RENDER_STATUS for pushId {push_id:#04x} within {args.render_timeout:.0f}s "
                "-- the screen may have updated but nothing told this client"
            )
        settle_s = time.perf_counter() - started - wire_s
        print(f"  RENDER_STATUS: {RENDER_RESULTS.get(result, result)} ({settle_s:.2f}s after the wire finished)")
        if result != RENDER_DISPLAYED:
            raise SystemExit(f"render did not succeed: {RENDER_RESULTS.get(result, result)}")


def _fill_body(size: int) -> bytes:
    """Deterministic printable filler of exactly `size` bytes.

    Content is irrelevant to BLE transfer timing (ATT carries raw bytes, no
    compression), so any text works; a repeated sentence is easier to eyeball
    in a screenshot than "x" * size if this is ever cross-checked visually.
    """
    unit = b"Batch timing measurement filler text for CompanionBatchModel investigation. "
    reps = (size // len(unit)) + 1
    return (unit * reps)[:size]


async def measure_idle_gap(session: Session, console, args) -> None:
    """Discriminator for src/CompanionBatchModel.cpp's 3s safety net (see
    src/CompanionBatchModel.cpp:64) that puts NO chunk traffic in the window,
    unlike measure_batch_timing() above.

    lastFieldMs_ is written only on a field's END (CompanionBatchModel.cpp
    :50-55), never per-CHUNK, so the safety net's clock is blind during a
    single large field's own CHUNK stream. On hardware a 16384-byte body
    (5.7s, 92 chunks, ~60ms apart, write-with-response) did NOT trip the ERR
    line -- unexpected, since the clock should have gone stale well past 3s.
    Two explanations remain, and this mode's actual experiment (a genuinely
    silent gap, no chunks at all) is what tells them apart:

      (A) poll() is not running/logging in that window at all -- the safety
          net is broken/starved outright, independent of the blind-clock
          reasoning.
      (B) poll() runs fine and something not yet found refreshes the clock
          during chunk traffic -- the blind-clock code reading is wrong.

    Interpretation (also printed as a legend in the report):
      - ERR fires on a >3s idle gap but did NOT fire on the 5.7s continuous-
        chunk body => poll() works; chunk traffic somehow keeps the batch
        alive => (B).
      - ERR fires on neither => the safety net never fires at all => (A),
        the 3s net is broken outright.
      - ERR fires on both => the earlier no-fire run was a fluke; rerun for
        flakiness before trusting either explanation.

    Sequence per rep: push FIELD_TITLE (final=False) -- sleep `gap` seconds
    with NO BLE traffic at all -- push FIELD_BODY (final=False) -- push
    FIELD_CONTENT_ID (final=True). The gap is the safety net's actual stated
    purpose (a client that has gone silent mid-batch), so this is what it is
    supposed to catch, not an edge case of it.
    """
    gaps = [float(g) for g in args.idle_gap_seconds.split(",") if g]
    print(
        f"\n=== Idle-gap measurement: gaps={gaps}s reps={args.measure_reps} ===\n"
        "  Legend: ERR on idle-gap but not on a continuous-chunk body => (B) poll() works,\n"
        "          chunk traffic keeps lastFieldMs_ fresh somehow.\n"
        "          No ERR on any gap => (A) the safety net never fires -- broken outright.\n"
        "          ERR on both idle-gap and continuous-chunk body => rerun, earlier no-fire was a fluke.\n"
        "  Raw counts only below -- no verdict is inferred here.\n"
    )
    console.pop_errors()  # discard anything pending before this mode starts

    for gap in gaps:
        fires = 0
        slept_samples: list[float] = []
        for rep in range(args.measure_reps):
            console.pop_errors()
            title_bytes = f"idle-gap {gap}s rep{rep}".encode("utf-8")
            body_bytes = f"idle-gap body rep{rep}".encode("utf-8")

            await session.push_field(FIELD_TITLE, title_bytes, push_id=0, final=False)

            # The silent window itself: no writes, no reads, no notifications
            # sent from this side. companion_protocol.py issues no periodic
            # traffic of its own (no keepalive/heartbeat loop exists in that
            # module -- verified by inspection), and bleak's notification
            # handlers here (Link._handle_session/_handle_button) only record
            # and dispatch, they never write back. So this sleep is a genuine
            # silent gap at the app layer; anything the BLE stack itself does
            # underneath (link-layer empty PDUs at each connection interval)
            # is outside this script's control and is not app traffic.
            slept_start = time.perf_counter()
            await asyncio.sleep(gap)
            slept_samples.append(time.perf_counter() - slept_start)

            await session.push_field(FIELD_BODY, body_bytes, push_id=0, final=False)
            await session.push_field(FIELD_CONTENT_ID, b"measure-idle-gap", final=True, push_id=0)

            # Same settle rationale as measure_batch_timing(): poll() runs on
            # the device's main loop task, not synchronously with the BLE
            # write completing, so give its LOG_ERR a moment to land before
            # reading the console.
            await asyncio.sleep(0.6)
            errs = console.pop_errors()
            fired = [e for e in errs if "commit flag missed" in e]
            if fired:
                fires += 1
                print(f"    [gap={gap}s rep {rep}] ERR fired: {fired}")
            else:
                print(f"    [gap={gap}s rep {rep}] no ERR")

            await asyncio.sleep(0.3)  # brief settle before the next rep's title lands

        n = len(slept_samples)
        avg_slept = sum(slept_samples) / n if n else 0.0
        print(
            f"  gap={gap:6.2f}s  ERR fired {fires}/{args.measure_reps} reps  "
            f"(actual sleep avg {avg_slept:.3f}s)"
        )


async def measure_batch_timing(session: Session, console, args) -> None:
    """Measures the title-END -> body-END wall-clock gap and the inter-CHUNK
    gap distribution within the body field, for both Write-Without-Response
    and Write-With-Response, sweeping body sizes.

    This is read-only with respect to firmware behaviour: it pushes ordinary
    title+body+content-id batches (pushId 0, no RENDER_STATUS awaited) and
    times them from the host, cross-referencing the device's own serial log
    (via `console`, the CMD:/CT: console) for the
    "content batch commit flag missed after 3000 ms" [ERR] line.

    Exists to test a specific hypothesis about src/CompanionBatchModel.cpp's
    lastFieldMs_: it is written only on a field's END (CompanionBatchModel.cpp:26,
    :54), never per-CHUNK (the only call site, CompanionModeActivity.cpp:148,
    is reached only from CompanionBle.cpp's END-handling branches at lines
    1659/1695/1699/1704) — so poll()'s 3s timeout is blind to activity within
    a single large field's own CHUNK stream. This does not change or work
    around that; it only measures it.

    MEASURED, and the hypothesis did NOT hold as a practical defect. On
    d5e1508f the safety net fires exactly as designed on a genuinely silent
    gap (--measure-idle-gap: 0/3 at 2.5s, 3/3 at 3.5s/5s/8s), so poll() is
    neither starved nor broken. What bounds the blind spot is
    kMaxFieldLen = 4096 (src/CompanionBle.h:51), truncated at START
    (CompanionBle.cpp:1378): a field that respects the cap transfers in
    0.78s measured, 1.18s at the slowest throughput seen here (3474 B/s),
    against a 3000ms threshold. A cap-respecting client cannot cross it, so
    do NOT "fix" kMaxTimeoutMs or move the clock to chunk granularity on the
    strength of the reasoning above -- the number, not the story, is why.

    Still genuinely open, and only reachable by a client that ignores the
    cap: a 16384-byte body (4x over) took 5.7s at the 60ms-interval profile
    and still did not trip the net, which the code above does not explain.
    Over-cap pushes are also where a real FIELD_SEQ_GAP was observed under
    Write-Without-Response flooding. Neither is a shape any of this
    project's own apps produce.
    """
    sizes = [int(s) for s in args.measure_sizes.split(",") if s]
    write_types = [w.strip() for w in args.measure_write_types.split(",") if w.strip()]
    print(
        f"\n=== Batch timing measurement: sizes={sizes} write_types={write_types} "
        f"reps={args.measure_reps} ==="
    )
    console.pop_errors()  # discard anything pending before this mode starts

    for wt in write_types:
        wwr = wt == "wwr"
        for size in sizes:
            gap_samples: list[float] = []
            chunk_gap_samples: list[float] = []  # seconds, flattened across reps
            err_fires = 0
            body_bytes = _fill_body(size)
            for rep in range(args.measure_reps):
                console.pop_errors()
                title_bytes = f"measure {wt} {size}B rep{rep}".encode("utf-8")

                chunk_ts: list[float] = []

                def progress(index: int, total: int, _ts=chunk_ts) -> None:
                    _ts.append(time.perf_counter())

                await session.push_field(FIELD_TITLE, title_bytes, push_id=0, chunk_response=not wwr)
                title_end = time.perf_counter()
                await session.push_field(
                    FIELD_BODY, body_bytes, push_id=0, progress=progress, chunk_response=not wwr
                )
                body_end = time.perf_counter()
                await session.push_field(
                    FIELD_CONTENT_ID, b"measure-batch-timing", final=True, push_id=0, chunk_response=not wwr
                )

                gap = body_end - title_end
                gap_samples.append(gap)
                if len(chunk_ts) >= 2:
                    chunk_gap_samples.extend(b - a for a, b in zip(chunk_ts, chunk_ts[1:]))

                # Let the device's poll() loop and its LOG_ERR (if any) land
                # before reading the console -- poll() runs on the main loop
                # task, not synchronously with the BLE write completing.
                await asyncio.sleep(0.6)
                errs = console.pop_errors()
                fired = [e for e in errs if "commit flag missed" in e]
                if fired:
                    err_fires += 1
                    print(f"    [{wt} {size}B rep {rep}] gap={gap:.3f}s -- ERR fired: {fired}")
                else:
                    print(f"    [{wt} {size}B rep {rep}] gap={gap:.3f}s")

                await asyncio.sleep(0.3)  # brief settle before the next rep's title lands

            gap_samples.sort()
            n = len(gap_samples)
            print(
                f"  {wt:8s} {size:6d}B  title->body gap  min={gap_samples[0]:.3f}s "
                f"median={statistics.median(gap_samples):.3f}s max={gap_samples[-1]:.3f}s  "
                f"throughput={size / statistics.median(gap_samples):.0f} B/s  "
                f"ERR fired {err_fires}/{n} reps"
            )
            if chunk_gap_samples:
                chunk_gap_samples.sort()

                def pct(p: float) -> float:
                    idx = min(len(chunk_gap_samples) - 1, int(round(p * (len(chunk_gap_samples) - 1))))
                    return chunk_gap_samples[idx]

                print(
                    f"  {wt:8s} {size:6d}B  inter-CHUNK gap (last size's reps, {len(chunk_gap_samples)} samples)  "
                    f"min={chunk_gap_samples[0] * 1000:.1f}ms median={pct(0.5) * 1000:.1f}ms "
                    f"p95={pct(0.95) * 1000:.1f}ms max={chunk_gap_samples[-1] * 1000:.1f}ms"
                )


# --------------------------------------------------------------------------- #
# Main flow
# --------------------------------------------------------------------------- #


async def run(args) -> None:
    print("Scanning for companion device...")
    devices = await BleakScanner.discover(timeout=6.0, service_uuids=[SERVICE_UUID])
    if not devices:
        print("No companion device found advertising the service. Is it in Companion Mode")
        print("(not already connected to a phone) and awake?")
        sys.exit(1)

    device = devices[0]
    print(f"Found: {device.name} ({device.address})")

    async with BleakClient(device.address) as client:
        caps = parse_capabilities(
            bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)),
            minimum_version=PROTOCOL_VERSION,
            who="this script (its UI declaration carries v12's mandatory content shape)",
        )
        print(
            f"  v{caps['version']} device {caps['device_id']}: {caps['px_wide']}x{caps['px_high']}px, "
            f"{caps['chars_wide']}x{caps['chars_high']} chars, icons {caps['icon_w']}x{caps['icon_h']}, "
            f"max image {caps['max_image']}B, {caps['max_sessions']} sessions"
        )

        link = Link(client, caps)
        link.on_button = report_button
        print(f"  ATT MTU {client.mtu_size}, chunk payload {link.chunk_payload_size(FIELD_CONTENT_ID)}B")
        await link.start_notify()

        session = Session(SCRIPT_APP_ID, DISPLAY_NAME, install_id_bytes=install_id())
        session.on_pending = report_pending
        session.on_background = report_background
        session.on_chunk_ack = report_chunk_ack
        session.on_field_seq_gap = report_field_seq_gap
        link.attach(session)

        token = None if args.forget else stored_token(caps["device_id"])
        if args.forget:
            print("  --forget: presenting no token, expect a pairing prompt.")
        reply = await session.hello(token)
        if not reply.ok:
            raise SystemExit(f"HELLO denied: {reply.reason_text}")
        remember_token(caps["device_id"], session.token)
        print(f"  Paired. sessionId={session.session_id}, asset tags="
              f"{ {k: v.hex() for k, v in session.asset_tags.items()} }")

        # Push only what the device does not already have — the device compares
        # nothing, so staleness is this side's conclusion.
        pushing_image = bool(args.image or args.image_from)
        shape = SHAPE_IMAGE if pushing_image else SHAPE_TEXT
        ui = declaration(shape)
        if session.asset_tags.get(FIELD_UI_DECL) != ui[:4]:
            print(f"  declaring content shape {SHAPE_NAMES[shape]}")
            await push_asset(session, FIELD_UI_DECL, ui)
        else:
            print(f"  UI declaration already current (shape {SHAPE_NAMES[shape]}).")

        if args.icon:
            icon = encode_icon(args.icon, caps["icon_w"], caps["icon_h"])
            if session.asset_tags.get(FIELD_ICON) != icon[:4]:
                await push_asset(session, FIELD_ICON, icon)
            else:
                print("  icon already current.")

        outcome = await session.acquire()
        if outcome[0] != "foreground":
            reason = ACQUIRE_DENIED_REASONS.get(outcome[1], f"unknown ({outcome[1]:#04x})")
            raise SystemExit(f"ACQUIRE denied: {reason}")
        print("  Screen acquired.")

        if args.measure_batch_timing or args.measure_idle_gap:
            # Deliberately imported here, not at module scope: this is the
            # only mode of this script that needs a serial console, and
            # companion_e2e_test.py's Console class (its CMD:/CT: wrapper) is
            # the harness's own, not a fourth reimplementation of it -- see
            # CLAUDE.md's "Every fix starts red" / "extend the harness" rule.
            from companion_e2e_test import Console

            console = Console(args.measure_console_port)
            try:
                if args.measure_batch_timing:
                    await measure_batch_timing(session, console, args)
                if args.measure_idle_gap:
                    await measure_idle_gap(session, console, args)
            finally:
                console.close()
            return

        if pushing_image:
            if args.image:
                raw_bitmap = Path(args.image).read_bytes()
            else:
                raw_bitmap = dither_to_raw_bitmap(args.image_from, caps["px_wide"], caps["px_high"])
                print(f"  dithered to {len(raw_bitmap)} bytes of raw packed-2bpp")
            await push_image(session, caps, raw_bitmap)
        elif not args.no_text:
            await push_content(session, caps, args)

        if args.set_tag is not None:
            tag_id, state = args.set_tag
            await session.set_tag(tag_id, state)
            print(f"  Set tag {tag_id} to state {state} (state-only write).")

        if args.listen:
            print("Listening for button events. Ctrl-C to stop.")
            try:
                while True:
                    await asyncio.sleep(1)
            except asyncio.CancelledError:
                pass
        else:
            await asyncio.sleep(1)  # let the last write settle before the link drops


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--title", default=DEFAULT_TITLE)
    parser.add_argument("--body", default=None, help="Body text (default: a short placeholder)")
    parser.add_argument("--body-file", default=None, help="Read body text from this file instead of --body")
    parser.add_argument("--content-id", default="test-script", help="Opaque content-id (truncated to 32 bytes)")
    parser.add_argument(
        "--image", default=None,
        help="Push this file as-is: raw packed 2bpp, no header, bytesPerRow=ceil(width/4) x height bytes exactly",
    )
    parser.add_argument("--image-from", default=None, help="Dither this image here, then push it (needs Pillow)")
    parser.add_argument("--icon", default=None, help="Encode this image as the 1-bpp sleep-screen icon (needs Pillow)")
    parser.add_argument("--no-text", action="store_true", help="Handshake and push assets, but push no content")
    parser.add_argument(
        "--tag",
        nargs=2,
        type=int,
        metavar=("ID", "STATE"),
        default=None,
        help="Push tag ID to STATE atomically with the content (0 hidden, 1 outline, 2 filled)",
    )
    parser.add_argument(
        "--set-tag",
        nargs=2,
        type=int,
        metavar=("ID", "STATE"),
        default=None,
        help="Set tag ID to STATE with a standalone Status write, after any content push",
    )
    parser.add_argument("--listen", action="store_true", help="Stay connected and print button events")
    parser.add_argument("--forget", action="store_true", help="Present no token, forcing a fresh pairing prompt")
    parser.add_argument(
        "--await-render", action="store_true",
        help="After a text push, block for RENDER_STATUS and print the result and latency "
        "(image pushes already do this unconditionally)",
    )
    parser.add_argument(
        "--render-timeout", type=float, default=10.0,
        help="Seconds to wait for RENDER_STATUS when --await-render is set (default: 10)",
    )
    parser.add_argument(
        "--measure-batch-timing", action="store_true",
        help="Measurement mode for the CompanionBatchModel lastFieldMs_ investigation: sweeps body "
        "sizes and write types, reporting the title-END->body-END wall-clock gap and the "
        "inter-CHUNK gap distribution within the body field, cross-referenced against the "
        "device's [ERR] commit-timeout log line over --measure-console-port. Pushes no other "
        "content and does not await RENDER_STATUS.",
    )
    parser.add_argument(
        "--measure-idle-gap", action="store_true",
        help="Discriminator mode for the same CompanionBatchModel investigation: pushes "
        "FIELD_TITLE, sleeps a genuinely silent gap (no BLE traffic at all, unlike "
        "--measure-batch-timing's continuous chunks), then FIELD_BODY/FIELD_CONTENT_ID, "
        "sweeping --idle-gap-seconds and reporting whether the device's 3s commit-timeout "
        "safety net (the 'commit flag missed' [ERR] log line) fired. Separates 'poll() is "
        "starved/broken' from 'chunk traffic refreshes the clock somehow' -- see the "
        "docstring on measure_idle_gap() for the full interpretation.",
    )
    parser.add_argument(
        "--idle-gap-seconds", default="1,2.5,3.5,5,8",
        help="Comma-separated silent-gap lengths in seconds to sweep with --measure-idle-gap "
        "(default: 1,2.5,3.5,5,8, straddling the 3s safety-net threshold). NOTE: the device's "
        "own BLE link-layer supervision timeout for both conn-param profiles is 4s "
        "(CompanionConnPolicy::kConnTimeoutSessionUnits/kConnTimeoutIdleUnits, both 400 "
        "10ms-units -- src/CompanionConnPolicy.h:44,54); that is a link-layer heartbeat kept "
        "alive by empty PDUs at each connection interval regardless of application traffic, so "
        "it should not by itself disconnect on an app-silent gap, but it has not been verified "
        "against gaps above ~4s on real hardware -- treat sweep points past there as unverified "
        "if the link drops.",
    )
    parser.add_argument(
        "--measure-console-port", default=None,
        help="Serial port for the CMD:/CT: test console (required with --measure-batch-timing "
        "or --measure-idle-gap; needs an [env:test] build, see docs/companion-test-console.md)",
    )
    parser.add_argument(
        "--measure-sizes", default="1024,4309,16384",
        help="Comma-separated body sizes in bytes to sweep (default: 1024,4309,16384 -- 4309 is "
        "the exact byte size of the e2e harness's [flags] group 80-line paginated body)",
    )
    parser.add_argument("--measure-reps", type=int, default=3, help="Repetitions per size/write-type (default: 3)")
    parser.add_argument(
        "--measure-write-types", default="wwr,response",
        help="Comma-separated subset of wwr,response (default: both)",
    )
    args = parser.parse_args()

    if (args.measure_batch_timing or args.measure_idle_gap) and not args.measure_console_port:
        parser.error(
            "--measure-batch-timing/--measure-idle-gap need --measure-console-port "
            "(the CMD:/CT: serial console)"
        )

    # v12: a peer declares one content shape and may only push fields belonging
    # to it. Tag state (0x07) is a TEXT field, so asking for a tag alongside an
    # image is asking the device to answer RejectedShape. Refuse here, where the
    # message can say why, rather than on the wire.
    if (args.image or args.image_from) and (args.tag is not None or args.set_tag is not None):
        parser.error(
            "--tag/--set-tag are TEXT-shape fields and cannot be combined with an image push: "
            "this script declares IMAGE for --image/--image-from, and a peer may only push "
            "fields matching its declared shape (see docs/companion-display-protocol.md)"
        )

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
