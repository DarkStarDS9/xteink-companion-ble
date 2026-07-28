#!/usr/bin/env python3
"""
Drive a Companion Mode device over BLE from a dev machine, without a phone app.

Implements the client side of docs/companion-display-protocol.md v6: the HELLO
handshake, token persistence, the button map, icons, text and image pushes. This
is the fastest way to exercise the firmware — it does not depend on either iOS
app being ready, and it can produce inputs (a 40 KB image, a 10-page body, a
deliberately malformed asset) that are awkward to trigger from an app.

Usage:
    python scripts/push_companion_content.py                        # push default text
    python scripts/push_companion_content.py --body-file article.txt
    python scripts/push_companion_content.py --image photo.png      # pre-dithered 8-bit grayscale
    python scripts/push_companion_content.py --image-from photo.jpg # dither it here first (needs Pillow)
    python scripts/push_companion_content.py --icon icon.png        # 1-bpp sleep-screen icon (needs Pillow)
    python scripts/push_companion_content.py --listen               # stay connected, print button events
    python scripts/push_companion_content.py --forget               # drop the stored token, re-pair

The pairing token is kept in ~/.crosspoint_companion_tokens.json, keyed by the
device id, so the second run onward needs no on-device confirmation.

Requires `bleak` (see scripts/requirements.txt); `Pillow` only for --image-from
and --icon. On macOS, the first run prompts for Bluetooth permission for the
terminal/Python process.
"""

import argparse
import asyncio
import hashlib
import json
import os
import struct
import sys
import uuid
from pathlib import Path

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
(
    SESS_HELLO_OK,
    SESS_HELLO_PENDING,
    SESS_HELLO_DENIED,
    SESS_FOREGROUND,
    SESS_BACKGROUND,
    SESS_ACQUIRE_DENIED,
    SESS_ASSET_ACK,
    SESS_IMAGE_STATUS,
) = (0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88)

DENIED_REASONS = {
    0x00: "user rejected",
    0x01: "timed out",
    0x02: "no session slots",
    0x03: "malformed HELLO",
    0x04: "storage failure",
    0x05: "another pairing prompt is up",
}
ACQUIRE_DENIED_REASONS = {0x00: "no UI declaration stored", 0x01: "unknown session"}
ASSET_RESULTS = {0x00: "stored", 0x01: "rejected: size", 0x02: "rejected: format", 0x03: "rejected: storage"}
IMAGE_RESULTS = {0x00: "displayed", 0x01: "decode failed", 0x02: "rejected: size", 0x03: "storage failed"}
BUTTON_NAMES = {0: "BACK", 1: "CONFIRM", 2: "LEFT", 3: "RIGHT", 4: "UP", 5: "DOWN", 6: "POWER"}

ROUTING_NONE, ROUTING_REMOTE, ROUTING_PAGE_PREV, ROUTING_PAGE_NEXT, ROUTING_SLEEP = range(5)

# A stable appId for this script. It is a real app identity as far as the device
# is concerned — its own peer directory, its own icon tile.
SCRIPT_APP_ID = uuid.UUID("2f1d7b64-9c3e-4a55-8f21-0c7b5e9a3d10").bytes
DISPLAY_NAME = "Dev Pusher"

TOKEN_STORE = Path.home() / ".crosspoint_companion_tokens.json"

# Conservative chunk payload — comfortably under the ~182 usable bytes a 185-byte
# MTU leaves, minus the 2 bytes of v6 CHUNK framing.
CHUNK_PAYLOAD = 176

DEFAULT_TITLE = "Test Article: BLE Push"
DEFAULT_BODY = (
    "This is a test article pushed directly over BLE, without the phone "
    "app. Use --title/--body/--body-file to push your own content."
)

# What this script declares about its own UI. LEFT/RIGHT page the buffered body
# on-device; everything else is forwarded so --listen can print it. The tags are
# this script's own invention — the device defines none.
BUTTON_MAP = [
    (2, ROUTING_PAGE_PREV, "<"),
    (3, ROUTING_PAGE_NEXT, ">"),
    (1, ROUTING_REMOTE, "Save"),
    (0, ROUTING_REMOTE, "Back"),
    (4, ROUTING_REMOTE, ""),
    (5, ROUTING_REMOTE, ""),
]


# --------------------------------------------------------------------------- #
# Local token / identity storage
# --------------------------------------------------------------------------- #


def load_store() -> dict:
    if TOKEN_STORE.exists():
        try:
            return json.loads(TOKEN_STORE.read_text())
        except (OSError, ValueError):
            pass
    return {}


def save_store(store: dict) -> None:
    TOKEN_STORE.write_text(json.dumps(store, indent=2))
    os.chmod(TOKEN_STORE, 0o600)


def install_id() -> bytes:
    """Persisted like a real app would: regenerating it makes a brand new peer."""
    store = load_store()
    if "install_id" not in store:
        store["install_id"] = os.urandom(16).hex()
        save_store(store)
    return bytes.fromhex(store["install_id"])


def stored_token(device_id: str) -> bytes | None:
    token = load_store().get("tokens", {}).get(device_id)
    return bytes.fromhex(token) if token else None


def remember_token(device_id: str, token: bytes) -> None:
    store = load_store()
    store.setdefault("tokens", {})[device_id] = token.hex()
    save_store(store)


def asset_tag(body: bytes) -> bytes:
    """First 4 bytes of SHA-256 — a content hash, so downgrades re-push correctly."""
    tag = hashlib.sha256(body).digest()[:4]
    return b"\x00\x00\x00\x01" if tag == b"\x00\x00\x00\x00" else tag


TAGS = [(0, "Saved"), (1, "New")]


def encode_ui_declaration() -> bytes:
    body = bytes([len(BUTTON_MAP)])
    for button, routing, label in BUTTON_MAP:
        encoded = label.encode("utf-8")
        body += bytes([button, routing, len(encoded)]) + encoded
    body += bytes([len(TAGS)])
    for tag_id, label in TAGS:
        encoded = label.encode("utf-8")[:12]
        body += bytes([tag_id, len(encoded)]) + encoded
    return asset_tag(body) + body


def encode_tag_state(states) -> bytes:
    """states: list of (tagId, state). state 0 hidden / 1 outline / 2 filled."""
    out = bytes([len(states)])
    for tag_id, state in states:
        out += bytes([tag_id, state])
    return out


# --------------------------------------------------------------------------- #
# Wire helpers
# --------------------------------------------------------------------------- #


def parse_capabilities(raw: bytes) -> dict:
    if len(raw) < 23:
        raise SystemExit(
            f"Capability characteristic is {len(raw)} bytes, expected 23. "
            "This device is running pre-v6 firmware; flash a v6 build first."
        )
    caps = {
        "version": raw[0],
        "chars_wide": raw[1],
        "chars_high": raw[2],
        "max_text": struct.unpack_from("<H", raw, 3)[0],
        "flags": raw[5],
        "max_image": struct.unpack_from("<I", raw, 6)[0],
        "max_sessions": raw[10],
        "icon_w": raw[11],
        "icon_h": raw[12],
        "device_id": raw[13:17].hex(),
        "px_wide": struct.unpack_from("<H", raw, 17)[0],
        "px_high": struct.unpack_from("<H", raw, 19)[0],
        "max_content_id": raw[21],
        "gray_levels": raw[22],
    }
    if caps["version"] != 6:
        raise SystemExit(f"Device speaks protocol v{caps['version']}, this script speaks v6.")
    return caps


class Session:
    """The handshake and everything that needs a sessionId."""

    def __init__(self, client: BleakClient, caps: dict):
        self.client = client
        self.caps = caps
        self.session_id = 0
        self.asset_tags: dict[int, bytes] = {}
        self.hello_tag = int.from_bytes(os.urandom(2), "little") or 1
        self.loop = asyncio.get_running_loop()
        self.hello_future: asyncio.Future | None = None
        self.acquire_future: asyncio.Future | None = None
        self.asset_futures: dict[int, asyncio.Future] = {}
        self.image_future: asyncio.Future | None = None

    # -- notifications ------------------------------------------------------ #

    def on_session_notify(self, _sender, data: bytearray) -> None:
        opcode = data[0]

        if opcode in (SESS_HELLO_OK, SESS_HELLO_PENDING, SESS_HELLO_DENIED):
            tag = struct.unpack_from("<H", data, 1)[0]
            if tag != self.hello_tag:
                return  # somebody else's handshake on this shared link
            if opcode == SESS_HELLO_PENDING:
                print("  Device is asking the user to confirm. Press CONFIRM on the device.")
                return
            if opcode == SESS_HELLO_DENIED:
                reason = DENIED_REASONS.get(data[3], f"unknown ({data[3]:#04x})")
                self._resolve("hello_future", ("denied", reason))
                return
            session_id = data[3]
            token = bytes(data[4:20])
            count = data[20]
            tags = {}
            for i in range(count):
                offset = 21 + i * 5
                tags[data[offset]] = bytes(data[offset + 1 : offset + 5])
            self._resolve("hello_future", ("ok", session_id, token, tags))
            return

        if opcode == SESS_FOREGROUND:
            self._resolve("acquire_future", True)
        elif opcode == SESS_BACKGROUND:
            reasons = {0: "preempted", 1: "released", 2: "link lost"}
            print(f"  BACKGROUND ({reasons.get(data[2], data[2])})")
        elif opcode == SESS_ACQUIRE_DENIED:
            reason = ACQUIRE_DENIED_REASONS.get(data[2], f"unknown ({data[2]:#04x})")
            self._resolve("acquire_future", reason)
        elif opcode == SESS_ASSET_ACK:
            future = self.asset_futures.pop(data[2], None)
            if future and not future.done():
                self.loop.call_soon_threadsafe(future.set_result, (data[3], bytes(data[4:8])))
        elif opcode == SESS_IMAGE_STATUS:
            self._resolve("image_future", data[2])

    def _resolve(self, attr: str, value) -> None:
        future = getattr(self, attr, None)
        if future and not future.done():
            self.loop.call_soon_threadsafe(future.set_result, value)

    @staticmethod
    def on_button_notify(_sender, data: bytearray) -> None:
        if len(data) < 4:
            return
        session_id, header = data[0], data[1]
        duration = struct.unpack_from("<H", data, 2)[0]
        content_id = bytes(data[4:])
        name = BUTTON_NAMES.get(header & 0x0F, "?")
        final = " FINAL" if header & 0x80 else ""
        suffix = f" content-id={content_id!r}" if content_id else ""
        print(f"  [session {session_id}] {name} held {duration * 0.1:.1f}s{final}{suffix}")

    # -- handshake ---------------------------------------------------------- #

    async def hello(self, token: bytes | None) -> tuple:
        self.hello_future = self.loop.create_future()
        payload = bytes([SESS_HELLO]) + struct.pack("<H", self.hello_tag) + SCRIPT_APP_ID + install_id()
        payload += bytes([len(token) if token else 0]) + (token or b"")
        name = DISPLAY_NAME.encode("utf-8")[:24]
        payload += bytes([len(name)]) + name
        await self.client.write_gatt_char(SESSION_CHAR_UUID, payload, response=True)
        return await asyncio.wait_for(self.hello_future, timeout=40)

    async def acquire(self) -> None:
        self.acquire_future = self.loop.create_future()
        await self.client.write_gatt_char(SESSION_CHAR_UUID, bytes([SESS_ACQUIRE, self.session_id]), response=True)
        result = await asyncio.wait_for(self.acquire_future, timeout=10)
        if result is not True:
            raise SystemExit(f"ACQUIRE denied: {result}")
        print("  Screen acquired.")

    async def release(self) -> None:
        await self.client.write_gatt_char(SESSION_CHAR_UUID, bytes([SESS_RELEASE, self.session_id]), response=True)

    # -- content ------------------------------------------------------------ #

    async def push_field(self, field: int, data: bytes, final: bool = False, progress: bool = False) -> None:
        field_byte = field | (FINAL_FLAG if final else 0)
        start = bytes([OP_START, field_byte, self.session_id]) + struct.pack("<I", len(data))
        await self.client.write_gatt_char(CONTENT_CHAR_UUID, start, response=True)

        total = max(1, (len(data) + CHUNK_PAYLOAD - 1) // CHUNK_PAYLOAD)
        for index, offset in enumerate(range(0, len(data), CHUNK_PAYLOAD)):
            chunk = data[offset : offset + CHUNK_PAYLOAD]
            await self.client.write_gatt_char(
                CONTENT_CHAR_UUID, bytes([OP_CHUNK, self.session_id]) + chunk, response=True
            )
            if progress and (index % 25 == 0 or index == total - 1):
                print(f"\r  sending {index + 1}/{total} chunks", end="", flush=True)
        if progress:
            print()

        await self.client.write_gatt_char(CONTENT_CHAR_UUID, bytes([OP_END, self.session_id]), response=True)

    async def push_asset(self, field: int, payload: bytes) -> int:
        future = self.loop.create_future()
        self.asset_futures[field] = future
        await self.push_field(field, payload)
        result, tag = await asyncio.wait_for(future, timeout=15)
        print(f"  asset {field:#04x}: {ASSET_RESULTS.get(result, result)} (tag {tag.hex()})")
        return result

    async def push_image(self, png: bytes) -> int:
        if len(png) > self.caps["max_image"]:
            raise SystemExit(f"Image is {len(png)} bytes, device cap is {self.caps['max_image']}.")
        self.image_future = self.loop.create_future()
        await self.push_field(FIELD_IMAGE, png, final=True, progress=True)
        print("  Waiting for the device to develop it (decode + grayscale settle)...")
        result = await asyncio.wait_for(self.image_future, timeout=180)
        print(f"  image: {IMAGE_RESULTS.get(result, result)}")
        return result

    async def set_tag(self, tag_id: int, state: int) -> None:
        """State-only change to one declared tag, without re-pushing content."""
        await self.client.write_gatt_char(
            STATUS_CHAR_UUID, bytes([self.session_id, tag_id, state]), response=True
        )


# --------------------------------------------------------------------------- #
# Optional Pillow-backed encoders
# --------------------------------------------------------------------------- #


def dither_to_png(path: str, width: int, height: int) -> bytes:
    """Crop-to-fill, dither to the panel's 4 levels, encode 8-bit grayscale PNG.

    Deliberately simple — this is a bring-up tool, not the Polaroid app. The real
    aesthetic decision belongs to the phone app; the only contract this has to
    honour is that every pixel ends up as one of {0, 85, 170, 255}.
    """
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("--image-from needs Pillow: pip install Pillow")
    import io

    source = Image.open(path).convert("L")
    scale = max(width / source.width, height / source.height)
    resized = source.resize((max(1, round(source.width * scale)), max(1, round(source.height * scale))), Image.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    cropped = resized.crop((left, top, left + width, top + height))

    # Floyd-Steinberg onto a 4-entry palette, then map palette indices back to
    # the exact byte values the device buckets on (gray / 85).
    palette = Image.new("P", (1, 1))
    palette.putpalette([0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255] + [0] * (768 - 12))
    quantized = cropped.convert("RGB").quantize(palette=palette, dither=Image.FLOYDSTEINBERG)
    levels = [0, 85, 170, 255]
    out = Image.new("L", (width, height))
    out.putdata([levels[index] for index in quantized.getdata()])

    buffer = io.BytesIO()
    out.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


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
    return asset_tag(bytes(bits)) + bytes(bits)


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
        caps = parse_capabilities(bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)))
        print(
            f"  v{caps['version']} device {caps['device_id']}: {caps['px_wide']}x{caps['px_high']}px, "
            f"{caps['chars_wide']}x{caps['chars_high']} chars, icons {caps['icon_w']}x{caps['icon_h']}, "
            f"max image {caps['max_image']}B, {caps['max_sessions']} sessions"
        )

        session = Session(client, caps)
        await client.start_notify(SESSION_CHAR_UUID, session.on_session_notify)
        await client.start_notify(BUTTON_CHAR_UUID, Session.on_button_notify)

        token = None if args.forget else stored_token(caps["device_id"])
        if args.forget:
            print("  --forget: presenting no token, expect a pairing prompt.")
        reply = await session.hello(token)
        if reply[0] == "denied":
            raise SystemExit(f"HELLO denied: {reply[1]}")
        _, session.session_id, new_token, session.asset_tags = reply
        remember_token(caps["device_id"], new_token)
        print(f"  Paired. sessionId={session.session_id}, asset tags="
              f"{ {k: v.hex() for k, v in session.asset_tags.items()} }")

        # Push only what the device does not already have — the device compares
        # nothing, so staleness is this side's conclusion.
        declaration = encode_ui_declaration()
        if session.asset_tags.get(FIELD_UI_DECL) != declaration[:4]:
            await session.push_asset(FIELD_UI_DECL, declaration)
        else:
            print("  UI declaration already current.")

        if args.icon:
            icon = encode_icon(args.icon, caps["icon_w"], caps["icon_h"])
            if session.asset_tags.get(FIELD_ICON) != icon[:4]:
                await session.push_asset(FIELD_ICON, icon)
            else:
                print("  icon already current.")

        await session.acquire()

        if args.image or args.image_from:
            if args.image:
                png = Path(args.image).read_bytes()
            else:
                png = dither_to_png(args.image_from, caps["px_wide"], caps["px_high"])
                print(f"  dithered to {len(png)} bytes of PNG")
            await session.push_image(png)
        elif not args.no_text:
            body = Path(args.body_file).read_text(encoding="utf-8") if args.body_file else (args.body or DEFAULT_BODY)
            print("Pushing title + body + content-id as one atomic batch...")
            await session.push_field(FIELD_TITLE, args.title.encode("utf-8"))
            await session.push_field(FIELD_BODY, body.encode("utf-8"))
            # content-id carries the final flag: the device only commits and
            # redraws once this last field's END arrives, so title+body land
            # together instead of the headline updating first.
            if args.tag is not None:
                # Tags last, carrying the final flag: content and tag state then
                # commit in one redraw rather than the tag flipping separately.
                await session.push_field(FIELD_CONTENT_ID, args.content_id.encode("utf-8")[:32])
                await session.push_field(FIELD_TAG_STATE, encode_tag_state([tuple(args.tag)]), final=True)
            else:
                await session.push_field(FIELD_CONTENT_ID, args.content_id.encode("utf-8")[:32], final=True)
            print("  Pushed.")

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
    parser.add_argument("--image", default=None, help="Push this PNG as-is (must already be 8-bit grayscale, 4 levels)")
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
    args = parser.parse_args()

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
