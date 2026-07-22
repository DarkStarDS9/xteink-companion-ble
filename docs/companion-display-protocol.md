# Companion Display Protocol

A BLE GATT protocol for phone apps to push short text content (title + body) to
the device's screen and receive button presses back. It is app-agnostic by
design — nothing here assumes a specific companion app. Any client that
implements this doc can drive the device; the device has no concept of what
the pushed text means (an article summary, podcast notes, a notification,
etc).

This fork adds Companion Mode on top of upstream CrossPoint Reader. Companion
Mode is this firmware's sole normal-boot activity — the device boots straight
into it (see `docs/companion-mode-implementation-notes.md`); there is no
Home/reader entry path in normal operation.

## Status

v2, implemented and verified on real X3 hardware. This is a **BLE
peripheral/GATT-server role**, not something upstream CrossPoint or this
fork's `feat-bluetooth` branch already has — that branch's BLE code is a HID
*host* (X3 pairs to page-turner remotes as central), the opposite role from
what Companion Mode needs. See `docs/companion-mode-implementation-notes.md`
for the bring-up log; `src/CompanionBle.h`/`src/CompanionBle.cpp` are the
implementation.

## GATT service

```
Service UUID:                 7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001
Content characteristic:       7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Write Without Response)
Button-event characteristic:  7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001  (Notify)
Capability characteristic:    7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001  (Read)
Status characteristic:        7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Write Without Response)
```

The device advertises this service UUID whenever it is in Companion Mode and
not currently connected. It accepts exactly one central connection at a time.
The advertised local name is `<generic prefix> <4 hex digits>` (a short tail
of the device's eFuse MAC), so two devices running this firmware never show up
as identical rows in a phone's BLE picker — see `src/CompanionBle.h`'s
`kDeviceNamePrefix` doc comment.

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
content length, the device truncates and shows what fits. A fresh body push
also resets the device's read-later indicator (see Status characteristic
below) back to "not saved" — it's per-article state.

On the client side this is a small, fully synchronous send loop — there's no
ack per chunk. If reliable delivery matters, use "Write" (not "Write Without
Response") for the CHUNK packets so BLE's own link-layer ack applies.

## Button-event characteristic — receiving input

The device notifies a single byte whenever one of the mapped buttons is
pressed while Companion Mode is active and the phone is connected:

```
0x01  PLAY_PAUSE   (bottom BACK button)
0x02  PREV         (a side button — see note below)
0x03  NEXT         (the other side button)
0x04  READ_LATER   (bottom CONFIRM button)
```

`LEFT`/`RIGHT` (the other two bottom buttons) page through whatever body text
is currently buffered, entirely on-device — the device already has the full
text locally, so local paging needs no round trip, and pressing them never
produces a BLE event. `POWER` (sleep/wake) is likewise handled entirely
on-device.

**Side button (UP/DOWN) note**: which physical side of the device UP vs DOWN
is on is a hardware detail that isn't visible from firmware source (shared
X3/X4 binary) — see `kSideUpMeansPrev` in
`src/activities/companion/CompanionModeActivity.cpp`, a single named constant
that maps physical UP/DOWN to PREV/NEXT. If PREV/NEXT feel backwards on a
given device, that's the one line to flip.

The semantic meaning of these events (e.g. "toggle playback", "save for
later", "previous/next article") is entirely up to the client app — the
firmware only reports which physical button was pressed.

## Capability characteristic — introspection

A single read-only value clients can query instead of hardcoding assumptions
about the device:

```
byte 0:      protocol version (currently 2)
byte 1:      screen width in characters, at the font Companion Mode uses
byte 2:      screen height in characters (lines per page)
bytes 3..4:  max content length per field, in bytes (uint16, little-endian)
```

v2 changes from v1: the button-event byte values were replaced (CONFIRM/BACK
→ PLAY_PAUSE/PREV/NEXT/READ_LATER, see above) and the Status characteristic
was added. The byte layout above is unchanged from v1 — only the version
number and the button-event meanings changed.

## Status characteristic — acknowledgements from the phone

Phone → device, single byte. Currently one value:

```
0x01  READ_LATER_SAVED
```

Sent by the client after it has durably saved a READ_LATER button-press
notification (e.g. "article queued for later"). The device flips its
on-screen read-later indicator from outline to filled and redraws with a
no-flash differential refresh. The indicator resets to outline whenever a new
body is pushed (see Content characteristic above) — it reflects the
currently-displayed article's save state, not a running total.

## Reference implementation status

- Firmware: `src/CompanionBle.h`/`src/CompanionBle.cpp` implement the
  peripheral (service/characteristics, content reassembly, button-event
  notify, Status characteristic, capability characteristic).
  `src/activities/companion/CompanionModeActivity.{h,cpp}` implements the
  on-device screen (pagination, button routing, read-later icon). See
  `docs/companion-mode-implementation-notes.md` for the bring-up log.
- No firmware-side automated tests exist for this feature — this project has
  no on-target test harness. Verify manually: connect, push a multi-page
  body, page with LEFT/RIGHT, press each mapped button and confirm the client
  receives the right notification, write Status 0x01 and confirm the icon
  flips, disconnect/reconnect, leave the waiting screen idle past the
  idle-sleep timeout and confirm the device sleeps.
