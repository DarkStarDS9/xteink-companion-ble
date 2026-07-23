#!/usr/bin/env python3
"""
Push test content to a Companion Mode device over BLE, without a phone app.

Implements the client side of docs/companion-display-protocol.md's Content
characteristic (START/CHUNK/END framing, the 0x80 final-field flag for atomic
title+body+content-id pushes). Useful for on-device layout/rendering testing
when the paired phone isn't available, or to reproduce a specific body length
(e.g. a long multi-page article) that's awkward to trigger from the app.

Usage:
    python scripts/push_companion_content.py
    python scripts/push_companion_content.py --title "..." --body "..."
    python scripts/push_companion_content.py --body-file article.txt

Requires `bleak` (see scripts/requirements.txt). On macOS, the first run
prompts for Bluetooth permission for the terminal/Python process.
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001"
CONTENT_CHAR_UUID = "7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001"

OP_START, OP_CHUNK, OP_END = 0x01, 0x02, 0x03
FIELD_TITLE, FIELD_BODY, FIELD_CONTENT_ID = 0x01, 0x02, 0x03
FINAL_FLAG = 0x80

# Conservative chunk payload size — comfortably under the ~182 usable bytes
# a 185-byte MTU negotiation leaves (see the protocol doc's "Content-id
# budget" section for the MTU math).
CHUNK_PAYLOAD = 180

DEFAULT_TITLE = "Test Article: BLE Push"
DEFAULT_BODY = (
    "This is a test article pushed directly over BLE, without the phone "
    "app. Use --title/--body/--body-file to push your own content."
)


async def push_field(client: BleakClient, field: int, data: bytes, final: bool = False) -> None:
    n = len(data)
    field_byte = field | (FINAL_FLAG if final else 0)
    start = bytes([OP_START, field_byte, n & 0xFF, (n >> 8) & 0xFF])
    await client.write_gatt_char(CONTENT_CHAR_UUID, start, response=True)

    for i in range(0, n, CHUNK_PAYLOAD):
        chunk = data[i : i + CHUNK_PAYLOAD]
        await client.write_gatt_char(CONTENT_CHAR_UUID, bytes([OP_CHUNK]) + chunk, response=True)

    await client.write_gatt_char(CONTENT_CHAR_UUID, bytes([OP_END]), response=True)


async def push_content(title: str, body: str, content_id: str) -> None:
    print("Scanning for companion device...")
    devices = await BleakScanner.discover(timeout=6.0, service_uuids=[SERVICE_UUID])
    if not devices:
        print("No companion device found advertising the service. Is it in Companion Mode")
        print("(not already connected to a phone) and awake?")
        sys.exit(1)

    dev = devices[0]
    print(f"Found: {dev.name} ({dev.address})")

    async with BleakClient(dev.address) as client:
        print("Connected. Pushing title + body (atomic — see kFinalFieldFlag)...")
        await push_field(client, FIELD_TITLE, title.encode("utf-8"))
        await push_field(client, FIELD_BODY, body.encode("utf-8"))
        # content-id carries the final flag: the device only commits/redraws
        # once this last field's END arrives, so title+body land together.
        await push_field(client, FIELD_CONTENT_ID, content_id.encode("utf-8")[:32], final=True)
        print("Push complete.")
        await asyncio.sleep(1)  # let the write settle before the connection drops


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--title", default=DEFAULT_TITLE)
    parser.add_argument("--body", default=None, help="Body text (default: a short placeholder)")
    parser.add_argument("--body-file", default=None, help="Read body text from this file instead of --body")
    parser.add_argument("--content-id", default="test-script", help="Opaque content-id (truncated to 32 bytes)")
    args = parser.parse_args()

    if args.body_file:
        with open(args.body_file, "r", encoding="utf-8") as f:
            body = f.read()
    else:
        body = args.body if args.body is not None else DEFAULT_BODY

    asyncio.run(push_content(args.title, body, args.content_id))


if __name__ == "__main__":
    main()
