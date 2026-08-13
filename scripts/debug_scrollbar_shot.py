#!/usr/bin/env python3
"""One-off debug aid: push a scrollable ToDo list, move the cursor near the
bottom of the visible window, and dump CMD:SCREENSHOT to a PNG so the
scroll-thumb / cursor-selection-outline overlap can be inspected without a
phone or a camera. Not part of the regular harness -- throwaway, per the
investigation that found no existing scenario forces a scrollable list.

Usage: .venv-harness/bin/python scripts/debug_scrollbar_shot.py --port /dev/cu.usbmodem212401 --out /tmp/shot.png
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from PIL import Image

from companion_e2e_test import (
    APP_A,
    BTN_CONFIRM,
    BTN_DOWN,
    CAPABILITY_CHAR_UUID,
    Console,
    LIST_BINDINGS_DECL,
    Link,
    PROTOCOL_VERSION,
    Results,
    SERVICE_UUID,
    Session,
    ensure_declared_foreground,
)
from companion_protocol import encode_list_doc, parse_capabilities


def decode_framebuffer_to_image(fb: bytes, logical_width: int, logical_height: int) -> Image.Image:
    """Inverts the Portrait transform documented in companion_e2e_test.py's
    compute_expected_bw_framebuffer(): phyX = logical_y, phyY =
    (logical_width - 1) - logical_x. phys_rows = logical_width, stride =
    ceil(logical_height / 8). Returns a 1-bit PIL image in LOGICAL
    coordinates (logical_width x logical_height), black=set pixel.
    """
    stride = (logical_height + 7) // 8
    img = Image.new("1", (logical_width, logical_height), color=1)  # 1 = white
    px = img.load()
    for phy_y in range(logical_width):  # phys row
        row_off = phy_y * stride
        for phy_x in range(logical_height):  # phys col (bit)
            byte = fb[row_off + (phy_x >> 3)]
            bit = (byte >> (7 - (phy_x & 7))) & 1
            x = (logical_width - 1) - phy_y
            y = phy_x
            px[x, y] = bit
    return img


async def main_async(args) -> None:
    console = Console(args.port)
    if not console.ping():
        console.close()
        sys.exit("No response to CMD:CPING -- flash -e test first")
    print("Serial console live.")
    console.reset_peers()
    console.pop_errors()

    if args.address:
        address = args.address
    else:
        print("Scanning for the device...")
        devices = await BleakScanner.discover(timeout=8.0)
        found = None
        for d in devices:
            try:
                async with BleakClient(d.address, timeout=5.0) as probe:
                    if any(s.uuid == SERVICE_UUID for s in probe.services):
                        found = d
                        break
            except Exception:
                continue
        if found is None:
            console.close()
            sys.exit("No companion device advertising")
        address = found.address
        print(f"Found {found.name} ({address})")

    results = Results()
    async with BleakClient(address) as client:
        caps = parse_capabilities(
            bytes(await client.read_gatt_char(CAPABILITY_CHAR_UUID)),
            minimum_version=PROTOCOL_VERSION,
            who="debug script",
        )
        link = Link(client, caps)
        await link.start_notify()
        session = Session(APP_A, "Debug Scrollbar")
        link.attach(session)

        ok = await ensure_declared_foreground(session, console, results, LIST_BINDINGS_DECL, "debug")
        if not ok:
            console.close()
            sys.exit("could not get the list peer onto the screen")

        item_count = args.items
        doc = encode_list_doc([{
            "id": 1, "title": "Scrollbar Debug", "groups": [{
                "id": 1, "label": "", "items": [
                    {"id": 100 + i, "text": f"Item {i}", "checked": 0}
                    for i in range(item_count)
                ],
            }],
        }], revision=1)
        verdict = await session.push_list_doc(doc, push_id=1)
        print(f"push_list_doc verdict: {verdict}")

        for _ in range(args.down):
            console.press(BTN_DOWN)

        print("Taking screenshot...")
        fb = console.screenshot()
        print(f"Got {len(fb)} bytes; logical {caps['px_wide']}x{caps['px_high']}")

        img = decode_framebuffer_to_image(fb, caps["px_wide"], caps["px_high"])
        img.save(args.out)
        print(f"Saved {args.out}")

    console.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--address", default=None, help="Skip scanning, connect directly to this BLE address")
    parser.add_argument("--out", default="/tmp/scrollbar_debug.png")
    parser.add_argument("--items", type=int, default=30)
    parser.add_argument("--down", type=int, default=8)
    args = parser.parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
