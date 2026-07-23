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

v4, implemented and verified on real X3 hardware. This is a **BLE
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
byte 1:      field | flag    (low 7 bits: 0x01 = title, 0x02 = body, 0x03 = content-id;
                               top bit 0x80 = "final field of this atomic push", see below)
                                                              — START packets only
bytes 2..3:  total length    (uint16, little-endian)         — START packets only
bytes 1..N:  payload bytes                                    — CHUNK packets only
(no payload)                                                    — END packets
```

A push of one field (title, body, or content-id) is: one `START` packet
declaring which field and its total byte length, one or more `CHUNK` packets
carrying the bytes in order (each chunk sized to fit the negotiated MTU minus
1 byte of framing overhead), then one `END` packet. Fields are independent
pushes over the same characteristic — send one field's full START/CHUNK…/END
before starting the next. The device does not assume an order beyond "each
field is internally ordered."

The device buffers a field's bytes as CHUNKs arrive and considers it
complete on END. If title/body exceeds the capability characteristic's
advertised max content length, the device truncates and shows what fits;
content-id has its own, much smaller cap (see below) and is truncated the
same way. A fresh body push also resets the device's read-later indicator
(see Status characteristic below) back to "not saved" — it's per-article
state.

### Atomic multi-field pushes (`0x80` final-field flag, v4)

By default, each field becomes visible independently the instant its own END
arrives — if a client pushes title then body as two separate field pushes,
the title can render (and be seen on screen) before the body finishes
transmitting, since body is typically much larger and takes measurably
longer over BLE. To push multiple fields so they always appear together, set
the top bit (`0x80`) of the **last** field's START byte in the batch. The
device keeps reassembling/buffering every field as usual, but only commits
(applies the buffered fields and redraws) when it processes an END whose
START carried this bit — so e.g. title, then body, then a content-id push
with `field = 0x03 | 0x80` renders title+body together instead of the
headline updating first. A client that doesn't need atomicity across fields
can simply never set this bit, which reproduces the pre-v4 per-field
behavior exactly.

This only brackets atomicity within a single push over the Content
characteristic — it does not coordinate with the Status characteristic or
across reconnects. If the client disconnects mid-batch before sending the
final-flagged field, the device applies whatever was buffered so far after a
short timeout, rather than getting stuck showing stale content indefinitely.

On the client side this is a small, fully synchronous send loop — there's no
ack per chunk. If reliable delivery matters, use "Write" (not "Write Without
Response") for the CHUNK packets so BLE's own link-layer ack applies.

### Content-id field (0x03) — opaque correlation token

Title and body are what's shown on screen; content-id is neither — it's an
**opaque byte blob the client defines and the device never interprets**,
pushed via the exact same START/CHUNK/END framing. The device remembers only
the most recently completed content-id push and echoes it back, appended
verbatim, on every button-event notification (see below) — so a client that
also pushes a fresh content-id alongside every new title/body can detect "this
button press was for an article I've since replaced" (a reconnect race, a
fast article skip, etc.) and no-op instead of acting on stale on-screen state.
The device does not reset content-id when a new body arrives (unlike the
read-later indicator) — a client that cares about this correlation should push
a new content-id with every title/body update it makes.

Max length: **32 bytes** (`kMaxContentIdLen` in `src/CompanionBle.h`),
independent of and much smaller than title/body's 4096-byte cap — see the
"Button-event characteristic" section below for why this ceiling is small.
There's no fixed layout inside those 32 bytes; treat it as a byte array, not a
typed value (a UUID, a hash, a short numeric string, etc. all fit).

## Button-event characteristic — receiving input

The device notifies whenever one of the mapped buttons is pressed while
Companion Mode is active and the phone is connected. The notification payload
is:

```
byte 0:      event            (0x01 = PLAY_PAUSE, 0x02 = PREV, 0x03 = NEXT, 0x04 = READ_LATER)
bytes 1..N:  content-id bytes (the most recently pushed content-id field, verbatim — 0 bytes if none pushed yet)
```

```
0x01  PLAY_PAUSE   (bottom BACK button)
0x02  PREV         (a side button — see note below)
0x03  NEXT         (the other side button)
0x04  READ_LATER   (bottom CONFIRM button)
```

There is no length prefix on the content-id bytes — the GATT notification's
own value length delimits it (`payload length - 1` bytes). A client that
doesn't use content-id can simply ignore any bytes after byte 0.

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
given device, that's the one line to flip. UP/DOWN presses are intentionally
not accompanied by an on-screen hint (unlike the bottom PLAY_PAUSE/READ_LATER/
page-turn hints) — they still work and still notify PREV/NEXT.

The semantic meaning of these events (e.g. "toggle playback", "save for
later", "previous/next article") is entirely up to the client app — the
firmware only reports which physical button was pressed.

### Content-id budget

The device requests MTU 185 (`NimBLEDevice::setMTU(185)`); ATT overhead is
always 3 bytes, so the practically available notification payload today is
~182 bytes, i.e. up to ~181 bytes for content-id after the event byte. That is
**not** the number to design against, though: this protocol is meant to stay
usable by other app-agnostic clients that may negotiate a smaller MTU, and
BLE's guaranteed floor (a central that only supports the minimum MTU, 23) is
20 usable ATT bytes — 19 bytes for content-id in the worst case. The firmware
enforces its own hard cap of 32 bytes (`kMaxContentIdLen`) regardless of
negotiated MTU: comfortably under the guaranteed floor, while still large
enough for e.g. a UUID string or a short opaque token. Don't rely on the full
negotiated-MTU headroom for content-id — treat 32 bytes as the contract.

## Capability characteristic — introspection

A single read-only value clients can query instead of hardcoding assumptions
about the device:

```
byte 0:      protocol version (currently 4)
byte 1:      screen width in characters, at the font Companion Mode uses
byte 2:      screen height in characters (lines per page)
bytes 3..4:  max content length per field, in bytes (uint16, little-endian) — title/body only, not content-id
```

v4 changes from v3: added the `0x80` final-field flag on the Content
characteristic's START field byte for atomic multi-field pushes (see above).
The byte layout above is unchanged — only the version number, and the
Content characteristic's field byte gaining a high-bit flag, changed. A v3
client that never sets the bit is unaffected (identical wire behavior to
before).

v3 changes from v2: added the content-id content field (0x03) and the
content-id bytes appended to every button-event notification (see above). The
byte layout above is unchanged — only the version number, and the
button-event payload's length (now variable, previously always 1 byte),
changed.

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
  body, page with LEFT/RIGHT (page-turn hints appear only when that direction
  is actually pageable), push a long title and confirm it wraps onto up to 2
  lines instead of eliding, confirm no hint is drawn for the side UP/DOWN
  buttons (they should still notify PREV/NEXT), push a content-id then press
  a mapped button and confirm the client receives it appended after the event
  byte, push a *different* content-id and confirm a stale client-side
  comparison would reject the next button press, write Status 0x01 and
  confirm the icon flips, disconnect/reconnect while content is loaded and
  confirm the client's re-push lands (not stuck on "Waiting for phone"),
  leave the waiting screen idle past the idle-sleep timeout and confirm the
  device sleeps, push a new article (title+body+final-flagged content-id) and
  confirm headline and body change on screen together in one redraw rather
  than the headline updating first.
