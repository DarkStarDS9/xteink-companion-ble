# Companion Display Protocol

A BLE GATT protocol for phone apps to push short text content (title + body) to
the device's screen and receive button presses back. It is app-agnostic by
design — nothing here assumes a specific companion app. Any client that
implements this doc can drive the device; the device has no concept of what
the pushed text means (an article summary, podcast notes, a notification,
etc).

This fork adds Companion Mode on top of upstream CrossPoint Reader. It does
not replace the e-reader — Companion Mode is a separate screen the user
enters explicitly from Home.

## Status

Draft / v1, design-stage. This is a **new BLE peripheral/GATT-server role**,
not something upstream CrossPoint or this fork's `feat-bluetooth` branch
already has — that branch's BLE code is a HID *host* (X3 pairs to page-turner
remotes as central), the opposite role from what Companion Mode needs. See
`docs/companion-mode-implementation-notes.md` for exactly what that means and
what's left to build; `src/CompanionBle.h` is the interface contract so far,
with no implementation yet.

## GATT service

```
Service UUID:              7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001
Content characteristic:    7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Write Without Response)
Button-event characteristic: 7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001  (Notify)
Capability characteristic: 7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001  (Read)
```

The device advertises this service UUID whenever it is in Companion Mode and
not currently connected. It accepts exactly one central connection at a time.

## Content characteristic — pushing text

BLE clients can't assume a large MTU (iOS negotiates anywhere from ~185 to
~500 bytes; other platforms may negotiate less), so content is sent as a
short sequence of framed packets rather than one write.

Each packet written to the content characteristic has the shape:

```
byte 0:      opcode          (0x01 = START, 0x02 = CHUNK, 0x03 = END)
byte 1:      field           (0x01 = title, 0x02 = body)   — START packets only
bytes 2..3:  total length    (uint16, little-endian)         — START packets only
bytes 1..N:  payload bytes                                    — CHUNK packets only
(no payload)                                                    — END packets
```

A push of one field (title or body) is: one `START` packet declaring which
field and its total UTF-8 byte length, one or more `CHUNK` packets carrying
the UTF-8 bytes in order (each chunk sized to fit the negotiated MTU minus 1
byte of framing overhead), then one `END` packet. Title and body are two
independent field pushes over the same characteristic — send title's
START/CHUNK…/END, then body's START/CHUNK…/END. The device does not assume
an order beyond "each field is internally ordered."

The device buffers a field's bytes as CHUNKs arrive and considers it
complete on END, at which point it becomes visible (a partial START without
a matching END is a no-op and is discarded if a new START for the same field
arrives). If a field exceeds the capability characteristic's advertised max
content length, the device truncates and shows what fits.

On the client side this is a small, fully synchronous send loop — there's no
ack per chunk. If reliable delivery matters, use "Write" (not "Write Without
Response") for the CHUNK packets so BLE's own link-layer ack applies.

## Button-event characteristic — receiving input

The device notifies a single byte whenever `CONFIRM` or `BACK` is pressed
while Companion Mode is active and the phone is connected:

```
0x01  CONFIRM pressed
0x02  BACK pressed
```

`LEFT`/`RIGHT` (page navigation through whatever text is currently buffered)
and `POWER` (sleep/wake) are handled entirely on-device and never produce a
BLE event — the device already has the full text locally, so paging needs no
round trip. `UP`/`DOWN` are currently unused/reserved and also produce no
event.

The semantic meaning of CONFIRM/BACK (e.g. "tag this as read later", "skip to
next") is entirely up to the client app — the firmware only reports which
physical button was pressed.

## Capability characteristic — introspection

A single read-only value clients can query instead of hardcoding assumptions
about the device:

```
byte 0:      protocol version (currently 1)
byte 1:      screen width in characters, at the font Companion Mode uses
byte 2:      screen height in characters (lines per page)
bytes 3..4:  max content length per field, in bytes (uint16, little-endian)
```

## Reference implementation status

- Firmware: `src/CompanionBle.h` declares the peripheral interface this
  protocol maps to; no `.cpp` yet. `docs/companion-mode-implementation-notes.md`
  has the concrete TODO list (NimBLE server setup, content reassembly buffer,
  button-event wiring, on-device pagination reusing the reader's existing
  text-wrap path). Nothing here has been built — there's no PlatformIO
  toolchain in the environment this was authored in. Build (`pio run`) and
  flash before relying on any of it.
- No firmware-side automated tests exist for this feature — this project has
  no on-target test harness. Verify manually: connect, push a multi-page
  body, page with LEFT/RIGHT, press CONFIRM/BACK and confirm the client
  receives the notification, disconnect/reconnect.
