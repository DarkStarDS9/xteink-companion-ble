# Companion Display Protocol

A BLE GATT protocol for phone apps to drive the device's screen — pushing text
(title + body) and images, declaring what the physical buttons mean, and
receiving button presses back. It is app-agnostic by design: nothing here
assumes a specific companion app. Any client that implements this doc can
drive the device, and the device has no concept of what the pushed content
means (an article summary, a photo, podcast notes, a notification, etc).

This fork adds Companion Mode on top of upstream CrossPoint Reader. Companion
Mode is this firmware's sole normal-boot activity — the device boots straight
into it (see `docs/companion-mode-implementation-notes.md`); there is no
Home/reader entry path in normal operation.

## Status

**v6 — the current contract.** v6 is a **clean break**: the session handshake
is mandatory, and a client that pushes content without a valid session is
ignored. A v5 client will connect, push, and see nothing happen. Both sides
must be released together; see "v6 changes from v5" below for the migration
list.

This document is **authoritative** and is written first on purpose: consumer
apps are built against it while the firmware side lands. Where the firmware and
this document disagree, the firmware is wrong. Anything not yet flashed and
verified on hardware is listed at the end of
`docs/companion-mode-implementation-notes.md`.

This is a **BLE peripheral/GATT-server role**, not something upstream
CrossPoint or this fork's `feat-bluetooth` branch already has — that branch's
BLE code is a HID *host* (X3 pairs to page-turner remotes as central), the
opposite role from what Companion Mode needs. See
`docs/companion-mode-implementation-notes.md` for the bring-up log;
`src/CompanionBle.h`/`src/CompanionBle.cpp` are the implementation, and
`docs/companion-multi-app-design.md` is the design this version implements.

A reference client implementation of everything below ships in this repo:
`clients/swift/CompanionKit` (SwiftPM package, iOS/macOS) and
`scripts/push_companion_content.py` (dev-machine Python, `bleak`).

## GATT service

```
Service UUID:                 7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001
Content characteristic:       7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Write Without Response)
Button-event characteristic:  7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001  (Notify)
Capability characteristic:    7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001  (Read)
Status characteristic:        7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Write Without Response)
Session characteristic:       7c9c0005-3e4a-4b1a-9c1e-6d8a1f2b0001  (Write, Notify)   — new in v6
```

The device advertises this service UUID whenever it is in Companion Mode and
not currently connected. It accepts exactly one central connection at a time.
The advertised local name is `<generic prefix> <4 hex digits>` (a short tail
of the device's eFuse MAC), so two devices running this firmware never show up
as identical rows in a phone's BLE picker — see `src/CompanionBle.h`'s
`kDeviceNamePrefix` doc comment.

**One link, several apps.** A BLE peripheral gets one link per central
*device*, not per app. When two apps on the same phone connect to the same
peripheral, the OS shares one underlying connection: writes from both arrive
interleaved on the same characteristics, and every notification is delivered
to *both* apps. Nothing at the link layer distinguishes them. That single fact
is why v6 exists — app identity is declared in-band, as a **session**, and
every frame in both directions carries the `sessionId` it belongs to. See
`docs/companion-multi-app-design.md` §2.

---

## Sessions

### Identity

Three ids, all opaque to the firmware (it compares bytes and never parses):

| Id | Size | Chosen by | Meaning |
|---|---|---|---|
| `appId` | 16 bytes | the app author, once, hardcoded | which app. Identical across every install. Groups sleep-screen icons. |
| `installId` | 16 bytes | the app, randomly, on first run | which phone's copy of that app. Persist it; regenerating it makes a new peer that must re-pair. |
| `deviceId` | 4 bytes | the device (eFuse MAC tail) | which display. Read from the capability characteristic; key *your* per-device storage on it. |

A **peer** is the pair `(appId, installId)` — one phone's copy of one app. All
per-peer state on the device (auth token, UI declaration, icon, staging files) is
keyed by it.

**Store `installId` somewhere that dies with the app, not somewhere that
outlives it** — `UserDefaults` on iOS, not the Keychain. Keychain items survive
app deletion, so a user who deletes and reinstalls would silently re-attach to
the peer directory of an install that no longer exists, inheriting assets and a
token it never pushed. A fresh install *should* become a fresh peer and re-pair;
that is one prompt, once, and it is the honest outcome. (The pairing **token**
is the opposite case and does belong in the Keychain: it is per-device, and
losing it costs the user a prompt for no reason.)

### Session lifecycle

```
central connects
   -> subscribe to the Session and Button-event characteristics
   -> read the Capability characteristic
   -> write HELLO on the Session characteristic
   <- HELLO_PENDING (first time only, while the user confirms on-device)
   <- HELLO_OK  { sessionId, token, asset digests }
   -> push any asset whose digest differs from yours  (UI declaration is mandatory)
   -> ACQUIRE
   <- FOREGROUND
   ... push content, receive button events ...
   -> RELEASE  (or another session ACQUIREs, or the link drops)
   <- BACKGROUND
```

`sessionId` is one byte, assigned by the device, in the range `0x01..0x04`.
`0x00` is never a valid session and always means "none". A session lives for
the duration of the BLE link: on disconnect every session is destroyed and the
app must re-`HELLO` on reconnect. Session ids are **not** stable across
connections — never persist one. What persists is the token.

Session cap: 4 (advertised in the capability characteristic). A fifth `HELLO`
is denied with `NO_SESSION_SLOTS`.

### Screen ownership

There is one screen, so exactly one session is **foreground** at a time and
everything else is background.

- Only the foreground session may push content and receive button events.
  Content frames tagged with a background (or unknown) session id are
  **silently dropped**.
- `ACQUIRE` policy is **last requester wins, unconditionally.** The user just
  brought that app to the foreground on their phone; the firmware has no
  standing to second-guess that. The preempted session gets `BACKGROUND` with
  reason `PREEMPTED`.
- `ACQUIRE` is **rejected** if the peer has no stored UI declaration (see "UI
  declaration field" below) — `ACQUIRE_DENIED` with reason `NO_UI_DECLARATION`. Push the declaration,
  then retry.
- The device retains **no content for a background session**. On regaining the
  foreground an app re-pushes everything it wants shown. (Buffering per-session
  content would cost one reassembly buffer per session on a part with no room
  for a second one.)

#### `ACQUIRE` is asynchronous

`ACQUIRE` is a request, not a state change. **Wait for the reply before pushing
anything**; content sent between the request and the reply is dropped with no
diagnostic.

- Granted → `FOREGROUND` with your `sessionId`.
- Refused → `ACQUIRE_DENIED` with a reason. Today the only recoverable one is
  `NO_UI_DECLARATION`: push field `0x05`, then retry.

A `FOREGROUND` notification is also how you learn you got the screen *back*
after being preempted. The device retains nothing for a background session, so
**every `FOREGROUND` means re-push everything you want on screen**, not just the
first one.

#### Screen ownership is app intent, not app lifecycle

Do **not** wire `ACQUIRE`/`RELEASE` to the phone OS's foreground/background
notifications. Owning the screen means "I want the display to be showing my
content", which is a decision only the app can make:

- A reader that is only useful while you are looking at your phone can
  reasonably acquire and release with its own UI.
- An audio app whose entire point is the phone in a pocket, playing, with the
  article on the reader **should hold the screen while OS-backgrounded**.
  Releasing there would break the feature at exactly the moment it matters.
- A navigation app, a timer, anything that keeps doing useful work unattended:
  same.

`RELEASE` when you no longer want the display, and not before. `RELEASE` from a
session that is not foreground is a no-op.

> **Naming warning.** The protocol's `BACKGROUND` (this session no longer owns
> the screen) and the phone OS's "app is backgrounded" are unrelated concepts
> that will both want the name `isBackgrounded` in client code. They are not the
> same state and an app can be in either without the other. Name the protocol
> one after the screen — `hasScreen`, `screenState` — not after the lifecycle.

#### Preemption mid-transfer

If another session acquires the screen while you are partway through a
multi-frame push, **the partial field is discarded** — the device drops its
reassembly buffer (and deletes any partially staged image) at the moment of
handover, and your remaining `CHUNK`s are dropped by the `sessionId` check.

There is no separate notification for this: the `BACKGROUND` you receive *is*
the signal. Treat any in-flight push as lost when `BACKGROUND` arrives, and
re-push from the start on the next `FOREGROUND`.

### Handshake correlation (`helloTag`)

Because notifications are delivered to every app on the phone, `HELLO_OK` /
`HELLO_PENDING` / `HELLO_DENIED` cannot be addressed by `sessionId` — the
client does not have one yet, and two apps may be handshaking at the same
moment. Every `HELLO` therefore carries a client-chosen 2-byte `helloTag`, and
the device echoes it verbatim on all three replies. **A client MUST ignore any
`HELLO_*` notification whose `helloTag` is not its own.** Pick it randomly per
`HELLO`; it has no meaning beyond correlation and is not remembered after
`HELLO_OK`.

Once a session exists, `sessionId` does that job and `helloTag` is not used
again.

---

## Session characteristic — handshake and screen ownership

`7c9c0005-3e4a-4b1a-9c1e-6d8a1f2b0001` (Write, Notify). Subscribe to
notifications **before** writing `HELLO`. Every message is a single write or a
single notification — this characteristic is never chunked.

`HELLO` (up to 76 bytes) and `HELLO_OK` (up to 30 bytes) exceed BLE's minimum
MTU, so a handshake needs an ATT MTU of at least 80. The device requests 185
and both iOS and Android negotiate well above the floor in practice; a central
that cannot get past 23 cannot use v6 at all.

### Phone → device (write)

```
0x01 HELLO      helloTag[2]  appId[16]  installId[16]
                tokenLen:1  token[tokenLen]   (tokenLen 0 or 16)
                nameLen:1   name[nameLen]     (UTF-8, <= 24 bytes, may be empty)
0x02 BYE        sessionId
0x03 ACQUIRE    sessionId
0x04 RELEASE    sessionId
```

`name` is the display name shown on the pairing prompt and the "waiting for
<app>" screen — the app's user-visible name ("Snap2Ink"), not the peer's.
Longer names are truncated to 24 bytes on a UTF-8 boundary.

`BYE` destroys the session (and drops the foreground if it held it) without
disconnecting the link. Optional — disconnecting has the same effect. Useful
when one app on a shared link is done but the other is still using the device.

### Device → phone (notify)

```
0x81 HELLO_OK       helloTag[2]  sessionId:1  token[16]
                    assetCount:1  assetCount x { assetId:1  tag[4] }
0x82 HELLO_PENDING  helloTag[2]
0x83 HELLO_DENIED   helloTag[2]  reason:1
0x84 FOREGROUND     sessionId
0x85 BACKGROUND     sessionId  reason:1
0x86 ACQUIRE_DENIED sessionId  reason:1
0x87 ASSET_ACK      sessionId  assetId:1  result:1  tag[4]
0x88 IMAGE_STATUS   sessionId  result:1
```

```
HELLO_DENIED reason      0x00 USER_REJECTED     user pressed BACK on the prompt
                         0x01 TIMEOUT           no answer within ~30s
                         0x02 NO_SESSION_SLOTS  4 sessions already live on this link
                         0x03 MALFORMED         unparseable HELLO
                         0x04 STORAGE           SD unavailable / peer dir could not be created
                         0x05 BUSY              another pairing prompt is already on screen

BACKGROUND reason        0x00 PREEMPTED         another session acquired the screen
                         0x01 RELEASED          this session released it
                         0x02 LINK_LOST         (informational; not deliverable in practice)

ACQUIRE_DENIED reason    0x00 NO_UI_DECLARATION push field 0x05, then retry
                         0x01 UNKNOWN_SESSION   no such live session

ASSET_ACK result         0x00 STORED
                         0x01 REJECTED_SIZE     asset larger than the advertised limit
                         0x02 REJECTED_FORMAT   unparseable for that asset id
                         0x03 REJECTED_STORAGE  SD write failed

IMAGE_STATUS result      0x00 DISPLAYED
                         0x01 DECODE_FAILED     not a readable PNG / decode error
                         0x02 REJECTED_SIZE     exceeded max image length
                         0x03 STORAGE_FAILED    could not stage to SD
```

`ASSET_ACK` and `IMAGE_STATUS` are the two things a client genuinely cannot
work out for itself: whether the device stored the asset, and whether the
image decoded. Everything else about rendering is deterministic from what was
pushed.

### Pairing and tokens

The first `HELLO` from an unknown peer, or one carrying a token the device does
not recognise, raises an on-screen prompt: *"Pair with `<name>`? CONFIRM /
BACK"*. The device notifies `HELLO_PENDING` immediately so the app can show
"confirm on your device".

- **Confirmed** — the device creates the peer's directory, generates a random
  16-byte token, and replies `HELLO_OK` carrying it. **Store the token
  durably** (Keychain on iOS); it is what makes every later connect silent.
- **Rejected, or no input within ~30 seconds** — `HELLO_DENIED`.

Only one prompt is shown at a time. A second unknown peer that says `HELLO`
while a prompt is up is denied with `BUSY` and should retry once the user has
dealt with the first — stacking prompts would leave the user confirming an app
whose name is no longer on screen.

`BUSY` is the one refusal worth retrying; every other `HELLO_DENIED` reason is an
answer. **Retrying means reconnecting**, not re-sending `HELLO` on the same link:
there is no "start over on this connection" message, by design — one handshake
per link keeps the session table's lifetime trivially tied to the connection.

A known peer presenting its correct token gets `HELLO_OK` immediately, with no
prompt. That is the whole of the "connects automatically" relationship.

**The link is not encrypted and the token is sniffable.** This is a deliberate
decision (`docs/companion-multi-app-design.md` §5): the token is a capability
to skip a confirmation prompt, not a secrecy mechanism. It works identically on
iOS and Android and avoids the OS re-pairing failure modes BLE bonding brings.
Do not push anything confidential over this protocol. If that ever changes, LE
Secure Connections replaces the token without changing anything else here.

Unpairing is on-device only. There is no protocol opcode for it.

### Asset digests

`HELLO_OK` ends with a digest block listing, for each per-peer asset, the
opaque 4-byte tag the device currently holds — or four zero bytes if it holds
none:

```
assetCount : 1
assetCount x { assetId : 1, tag : 4 }
```

`assetId` values match the content field ids: `0x05` UI declaration, `0x06` icon.

The client compares each tag against the tag of the asset it would push, and
pushes only the ones that differ, as content fields `0x05` / `0x06`, each
prefixed with its 4-byte tag. **The device performs no comparison and computes
no hash** — it stores the bytes an app handed it and reads them back. Which
asset is stale is the app's conclusion, not the firmware's.

The tag is opaque, so a counter *works*, but **the recommended construction is
the first 4 bytes of SHA-256 over the asset body** (the bytes after the tag, not
including it). Every client in this repo uses exactly that, and an app that
follows it gets correct behaviour without thinking about it.

A counter breaks on app downgrade: the device holds a tag the older build will
never produce again, so it never re-pushes and the user is left on a control
scheme their app no longer implements. A content hash is correct in both
directions.

A tag of `00 00 00 00` on the wire means "no asset stored". Do not use it as a
real tag value; if your hash lands on it, push anything else (e.g. flip the low
bit).

**A peer with no UI declaration cannot take the screen** (see `ACQUIRE_DENIED`).
This makes "an app with undefined buttons" structurally impossible rather than
a case the rendering code has to handle.

---

## Content characteristic — pushing title, body, images and assets

BLE clients can't assume a large MTU (iOS negotiates anywhere from ~185 to
~500 bytes; other platforms may negotiate less), so content is sent as a
sequence of framed packets rather than one write.

### Framing (v6)

```
START:  byte 0      opcode = 0x01
        byte 1      field | final-flag   (low 7 bits = field id; 0x80 = "final field of this push")
        byte 2      sessionId
        bytes 3..6  total payload length, uint32 little-endian

CHUNK:  byte 0      opcode = 0x02
        byte 1      sessionId
        bytes 2..N  payload bytes

END:    byte 0      opcode = 0x03
        byte 1      sessionId
```

A push of one field is: one `START` declaring the field and its total byte
length, one or more `CHUNK`s carrying the bytes in order (each sized to the
negotiated MTU minus the 2 bytes of framing overhead), then one `END`. Fields
are independent pushes over the same characteristic — send one field's full
START/CHUNK…/END before starting the next. The device does not assume an order
beyond "each field is internally ordered".

**`sessionId` is on every frame, including `CHUNK`.** It costs one byte per
packet and buys the guarantee that a stray write from a background app can
never inject bytes into another app's in-flight transfer. The apps on this
protocol are cooperating, not adversarial — but they are written by different
codebases on different release cycles, which is the same failure mode.

Frames whose `sessionId` is not the current foreground session are dropped
without effect. A frame arriving with no session in the foreground is dropped.

A new `START` discards any partial reassembly in progress. Reassembly is also
discarded on disconnect and on a foreground handover.

### Field ids

| Id | Field | Cap | Reassembled into |
|---|---|---|---|
| `0x01` | title | max text length (capability) | RAM |
| `0x02` | body | max text length (capability) | RAM |
| `0x03` | content-id | 32 bytes | RAM |
| `0x04` | image | max image length (capability) | **streamed to SD**, never buffered in RAM |
| `0x05` | UI declaration (buttons + tags) | 512 bytes | SD (`ui.bin`) |
| `0x06` | icon | icon width x height / 8 bytes | SD (`icon.bin`) |
| `0x07` | tag state | 13 bytes | RAM (foreground only) |

Next free: `0x08`.

Content past a field's cap is truncated (title/body/content-id) or rejected
outright with `ASSET_ACK`/`IMAGE_STATUS` (image, UI declaration, icon) — a
truncated asset is worse than no asset.

### Atomic multi-field pushes (`0x80` final-field flag)

By default each field becomes visible independently the instant its own `END`
arrives — pushing title then body as two fields lets the title render before
the body finishes transmitting, since body is much larger and takes measurably
longer over BLE. To make several fields appear together, set the top bit
(`0x80`) of the **last** field's START field byte. The device keeps buffering
every field as usual but only commits (applies and redraws) when it processes
an `END` whose `START` carried the bit.

So: title, then body, then a `0x03 | 0x80` content-id push renders title and
body together in one redraw. A client that doesn't need atomicity simply never
sets the bit.

This brackets atomicity only within one sequence of pushes on the Content
characteristic — it does not coordinate with the Status characteristic or
across reconnects. If the client disconnects mid-batch before sending the
final-flagged field, the device applies whatever was buffered after a short
timeout (3 s) rather than sitting on stale content indefinitely.

On the client side this is a small, fully synchronous send loop — there is no
per-chunk ack. If reliable delivery matters, use "Write" (not "Write Without
Response") for the CHUNK packets so BLE's own link-layer ack applies. For the
image field, **always** use Write-with-response: a dropped chunk produces a
corrupt PNG and a wasted transfer.

### Content-id field (`0x03`) — opaque correlation token

Title and body are what's shown on screen; content-id is neither — it is an
**opaque byte blob the client defines and the device never interprets**, pushed
with the same framing. The device remembers only the most recently completed
content-id for the foreground session and echoes it back, verbatim, on every
button-event notification — so a client that pushes a fresh content-id
alongside every title/body can detect "this button press was for an article I
have since replaced" (a reconnect race, a fast skip) and no-op instead of
acting on stale on-screen state.

The device does not reset content-id when a new body arrives, any more than it
resets tags. A client that cares about this correlation should push a new
content-id with every title/body update.

Max length: **32 bytes** (`kMaxContentIdLen` in `src/CompanionBle.h`) — see
"Content-id budget" below for why it is so much smaller than title/body.

### Image field (`0x04`) — pre-dithered PNG

The client captures/crops a photo, **dithers it itself** to the panel's palette,
encodes it as PNG, and pushes it as field `0x04`. The device streams the bytes
straight to `peers/<peerKey>/data/incoming.png` on the SD card as they arrive —
it never holds the image in RAM — and on `END` decodes it to the framebuffer
with the firmware's own dither **disabled**, then runs the two-pass grayscale
settle. `IMAGE_STATUS` reports the outcome.

The transfer plus the grayscale settle takes several seconds. That is expected
and is not optimised for; a slow "developing" draw is thematically wanted.

**Quantization contract.** The device's non-dithered decode path buckets an
8-bit grayscale sample into four levels by integer division:

```c
level = gray / 85;   if (level > 3) level = 3;
```

For a client-side dither to land exactly where intended, **encode each already
dithered pixel as one of the four values `{0, 85, 170, 255}`** — not merely
"some value inside each bucket". Anything else re-quantizes unpredictably at
the boundaries.

**Format.** Grayscale PNG (colour type 0), not interlaced. **Bit depth 2 is
recommended**; depths 1, 4 and 8 also decode. Colour type 0 is the contract —
anything else may work and may stop working.

Use 2 bits per pixel unless you have a reason not to. It is exact and roughly
four times smaller:

- **Exact.** The decoder expands a packed sample with `sample * 255 / maxSample`,
  which for a 2-bit sample is `s * 255 / 3` = `{0, 85, 170, 255}` — precisely the
  values the subsequent `gray / 85` bucketing maps back to levels 0–3. There is
  no rounding anywhere on the round trip. Feeding 8-bit samples of the same four
  values is equally exact but says the same thing in four times the space.
- **Smaller, and that is what decides whether a print transfers quickly.** A full
  panel at 2 bpp is 96,000 bytes before compression against 384,000 at 8 bpp, and
  dithered noise is close to incompressible, so the ratio largely survives
  deflate. Measured on real dithered full-panel prints: Atkinson 28.5 KB,
  Floyd–Steinberg 34.9 KB, ordered Bayer 5.3 KB. At 8 bpp the error-diffusion
  cases are several times that, which is minutes of extra BLE transfer for no
  gain in fidelity.

The device validates bit depth in exactly one place and accepts 1/2/4/8 for
grayscale; there is no second check to trip over.

**Dimensions.** Encode at exactly the pixel size the capability characteristic
advertises (bytes 17..20). **Do not assume a panel size** — a measured X3 in
Companion Mode reports **528 x 792**, not the 800 x 480 that older notes in this
repo assume. That mismatch is exactly why these bytes exist; read them. The device centres the image and, if it is larger
than the screen in either axis, scales it down to fit — which resamples and
therefore *destroys the dither you carefully applied*. Smaller images are
centred, not scaled up. Rotation and cropping are the phone's job; the device
never rotates.

**Interaction with text.** An image push replaces the screen entirely — title,
body and paging are not drawn while an image is displayed. Pushing a body
afterwards returns the screen to text. There is no compositing of the two, and
no "image mode" the client enters or leaves: the last completed push of either
kind is what is on screen.

**Tags are drawn over an image, but only if you switch one on.** A visible tag
is rendered as a chip in the top corner of the print, over the image content —
there is nowhere else for it to go, since reserving a band would shrink the
image and force the scaling that ruins your dither.

So the choice is yours and it is explicit: leave every tag hidden and the print
is pixel-exact, or switch one on and accept the pixels the chip costs. The
device does not decide this for you, and it does not silently drop tags on the
image screen either — the earlier behaviour, where tag state was accepted,
acknowledged and then never drawn on an image, was a silent no-op and is gone.

### UI declaration field (`0x05`)

Everything the app declares about its own on-device UI: what its buttons do and
what they are called, and what tags exist and what they are called. One asset,
one digest.

```
bytes 0..3   asset digest (opaque, stored verbatim — see "Asset digests")

byte 4       button entry count N
N x {  buttonId : 1
       routing  : 1
       labelLen : 1
       label    : labelLen bytes, UTF-8, may be empty  }

byte         tag entry count M          <- optional; absent means "no tags"
M x {  tagId    : 1
       labelLen : 1
       label    : labelLen bytes, UTF-8  }
```

**Why one asset and not two.** Buttons and tags are the same kind of thing — near
static strings the device stores and draws without understanding — and they
change on the same cadence, at app update. Folding them halves the digest
bookkeeping every client has to do, keeps `HELLO_OK`'s digest block at two
entries, and means an app writes one encoder instead of two. The cost is that
relabelling a tag re-pushes the buttons too, which is a few hundred bytes once
per app version.

An app with no tags may simply stop after its buttons; the tag count byte is
optional. An app with no buttons still needs this asset — `ACQUIRE` is gated on
it existing.

`buttonId` uses the same values as the button-event characteristic (table
below). A button not listed behaves as `NONE`.

`routing` is a small **closed** enum — the complete set of things the firmware
can do by itself, closed on purpose:

```
0x00  NONE               no hint drawn, no notification — button is dead in this app
0x01  REMOTE             notify the phone (raw button id + hold duration)
0x02  LOCAL_PAGE_PREV    page the locally-buffered body backward
0x03  LOCAL_PAGE_NEXT    page the locally-buffered body forward
0x04  LOCAL_SLEEP        sleep the device
```

Notes:

- `POWER` (`0x06`) is **not remappable** — an entry for it is ignored. It stays
  firmware-owned in every app so a wedged app can never make the device
  un-sleepable.
- The label is an opaque string the firmware only draws. It carries no meaning
  to the device: a `REMOTE` button labelled "Shutter" is still just "notify raw
  button id 1". Labels for bottom buttons appear in the on-screen hint row;
  labels for the side buttons (`UP`/`DOWN`) are stored but not currently drawn
  (there is no hint position for them).
- There is **no default map.** v5's implicit fallback (LEFT/RIGHT page locally,
  everything else notifies) does not survive — an app declares its scheme or
  does not get the screen. The firmware keeps a fixed internal scheme only for
  its own screens: the pairing prompt, "waiting for <app>", and the sleep grid.
- Persisted to `ui.bin` in the peer directory, so the hints stay correct while
  disconnected.
- Ship a new tag when an app update changes the scheme; the device picks it up
  on the next connect without re-pairing.

#### Tags

A tag is a short labelled chip the device draws beside the title, which the app
switches on and off at runtime. **The firmware defines no tags.** There is no
built-in "saved", no fixed slots, no reserved ids, no enum of permitted values —
the app declares which tags exist and what each is called, exactly as it declares
button labels, and the device stores the strings and renders them. This is the
same three-part shape as buttons: the app declares the set, the device persists
and draws it, and the runtime message carries only state.

- **`tagId` is per-peer.** It means whatever the declaring app says it means, and
  two apps both using id `0` never collide, because each peer's declaration is
  its own. Ids need not be contiguous or start at zero.
- Limits: **6 tags**, label **12 bytes** each. Longer labels are truncated on a
  UTF-8 boundary; tags past the sixth are dropped. These are what bound the
  device's per-peer RAM (90 bytes, resident only for the foreground peer).
- Every tag starts **hidden** when the declaration is loaded. Declaring a tag
  says it exists, not that it is on.
- A tag id that was never declared is ignored wherever it appears. The
  declaration is the only place tags come into existence.

Max 512 bytes total for the whole declaration, which is far more than 7 buttons
and 6 tags need.

### Tag state field (`0x07`)

```
byte 0       entry count K
K x {  tagId : 1
       state : 1  }
```

```
state   0x00 HIDDEN    declared but not drawn at all
        0x01 OUTLINE   drawn, unfilled
        0x02 FILLED    drawn, filled
```

Carries no labels and declares nothing — only which of the peer's already
declared tags are now in which state. Ids not in the message keep their current
state.

**This field exists so tag state can ride the `0x80` atomic batch.** Push it in
the same batch as title/body and the content and its tags commit in a single
redraw, so there is never a frame where new content wears the previous content's
tags:

```
title  (0x01)
body   (0x02)
tags   (0x07 | 0x80)     <- final: everything commits here, one redraw
```

That ordering problem is real and worth stating plainly: **tags are not reset by
a content push.** When a tag should clear is app meaning — the same reasoning
that keeps the firmware out of button semantics — so a device that cleared tags
because a body arrived would be interpreting. If you push content without tag
state, the previous content's tags stay on screen. Push both together.

Tags **are** cleared on a foreground handover, since they carry the outgoing
app's meaning and not the incoming one's, and on a fresh UI declaration.

### Icon field (`0x06`)

```
bytes 0..3   asset digest (opaque, stored verbatim)
bytes 4..N   1-bpp bitmap, row-major, MSB-first within each byte
```

Exactly `iconWidth x iconHeight / 8` bitmap bytes for the dimensions advertised
in the capability characteristic (64x64 = 512 bytes today). **A set bit is ink
(black); a clear bit is paper** — matching the 1-bpp framebuffer, so an icon
authored as a black-on-white bitmap needs no inversion. Any other length is
rejected with `ASSET_ACK(REJECTED_SIZE)`.

**The advertised icon width is always a multiple of 8**, and that is a
guarantee, not an accident of the current 64x64 value. It means the packed size
is exactly `width * height / 8` with no row padding, and a client never has to
invent a padding convention. If a future device ever wants a non-aligned width,
that is a protocol revision with an explicit padding rule, not a silent change.

Drawn on the sleep screen in a grid of every enrolled app, **grouped by
`appId`** — the same app paired from two phones is one tile, not two, and the
most recently enrolled install's icon wins. The currently-connected app's tile
is marked. Icons are read from SD one at a time into a 512-byte stack buffer at
render time and discarded; none are resident in RAM.

The grid is decorative, **not a launcher** — the device cannot start an app on
the phone, so a selectable grid would promise something it can't deliver. At
most 18 tiles are drawn (6 x 3), most-recently-seen `appId`s first.

---

## Button-event characteristic — receiving input

The device notifies whenever a button whose routing is `REMOTE` is pressed,
repeated about every 100 ms while it stays held, plus one final notification on
release — while the pushing session holds the foreground. Payload (v6):

```
byte 0:      sessionId        the foreground session this event belongs to
byte 1:      header           bit7 = isFinal
                              bits6-4 = event type (0x1 = ButtonPress; reserved otherwise)
                              bits3-0 = button id (table below)
bytes 2..3:  duration         uint16, little-endian, elapsed hold in 100 ms ticks
                              since the initial press (0 for the initial-down event)
bytes 4..N:  content-id bytes (the most recently pushed content-id, verbatim — 0 bytes if none)
```

**A client MUST check `sessionId` and ignore events for other sessions.** Both
apps on a shared link receive every notification.

```
0x00  BACK      (bottom BACK button)
0x01  CONFIRM   (bottom CONFIRM button)
0x02  LEFT      (bottom LEFT button)
0x03  RIGHT     (bottom RIGHT button)
0x04  UP        (a side button — see note below)
0x05  DOWN      (the other side button)
0x06  POWER     (never notified, firmware-owned, not remappable)
```

These ids mirror `HalGPIO::BTN_*`/`InputManager::BTN_*` exactly (see
`lib/hal/HalGPIO.h`) — the wire byte is the same index the firmware's own HAL
uses internally, not a protocol-specific renumbering.

Unlike v5, **which buttons reach the wire is now the app's decision**, declared
in its UI declaration. LEFT/RIGHT are no longer hardwired to local paging and
POWER is still never notified.

A press/hold/release sequence looks like this on the wire (BACK held for 1.5 s
by session 2, then released):

```
[session=2, button=BACK, isFinal=0]  duration=0    <- initial press
[session=2, button=BACK, isFinal=0]  duration=1    <- ~100ms held
[session=2, button=BACK, isFinal=0]  duration=2    <- ~200ms held
...
[session=2, button=BACK, isFinal=1]  duration=15   <- released at ~1.5s
```

A client MUST NOT rely on the `isFinal` notification alone to detect release —
a disconnect mid-hold means it may never arrive. Treat "no repeat notification
for noticeably longer than ~100 ms" as an implicit release too.

There is no length prefix on the content-id bytes — the GATT notification's own
value length delimits it (`payload length - 4` bytes, after the 4-byte
session+header+duration prefix). A client that doesn't use content-id can
ignore any bytes after byte 3.

**Side button (UP/DOWN) note.** Which physical side of the device UP vs DOWN is
on is a hardware detail not visible from firmware source (shared X3/X4 binary).
The firmware reports raw UP/DOWN as-is; a client that wants a consistent
PREV/NEXT feel across devices owns that decision itself (or exposes it as a
user-facing setting).

The semantic meaning of these events ("toggle playback", "save for later",
"hold N seconds to do X") is entirely up to the client app — the firmware only
reports which physical button fired and for how long it has been held.

### Content-id budget

The device requests MTU 185 (`NimBLEDevice::setMTU(185)`); ATT overhead is
always 3 bytes, so the practically available notification payload today is
~182 bytes, i.e. ~178 bytes for content-id after the 4-byte prefix. That is
**not** the number to design against: this protocol is meant to stay usable by
clients that negotiate a smaller MTU, and BLE's guaranteed floor (MTU 23) is 20
usable ATT bytes — 16 bytes for content-id in the worst case. The firmware
enforces a hard 32-byte cap (`kMaxContentIdLen`) regardless of negotiated MTU:
comfortably close to the guaranteed floor, while still large enough for a UUID
string or a short opaque token. Treat 32 bytes as the contract.

---

## Capability characteristic — introspection

A single read-only value clients query instead of hardcoding assumptions about
the device. **23 bytes** in v6:

```
byte 0        protocol version = 6
byte 1        screen width in characters, at the font Companion Mode uses
byte 2        screen height in characters (lines per page)
bytes 3..4    max text field length, uint16 LE — title/body only
byte 5        feature flags: bit0 image, bit1 UI declaration, bit2 icons, bit3 sessions
bytes 6..9    max image field length, uint32 LE
byte 10       max concurrent sessions (4)
byte 11       icon width in pixels
byte 12       icon height in pixels
bytes 13..16  deviceId — the 4-byte eFuse MAC tail, most-significant byte first
bytes 17..18  screen width in pixels, uint16 LE
bytes 19..20  screen height in pixels, uint16 LE
byte 21       max content-id length (32)
byte 22       image grey levels (4)
```

### Readable before the handshake — a compatibility guarantee

**This characteristic is unconditionally readable with no session.** It carries
no per-app state, it is never gated on `HELLO`, and that will not change in any
future revision. It is the one thing a client can rely on before it knows
whether it can talk to the device at all.

That makes it the graceful-degradation path across the v5 break, which is the
whole reason to guarantee it. A v6 client should:

1. Read the characteristic immediately after connecting.
2. Check byte 0. If it is not 6, tell the user *"this reader's firmware is too
   old for this version of <app>"* (or too new) and stop. Do not attempt the
   handshake, and do not guess at the layout — v6's 23-byte value shares nothing
   past byte 4 with v5's 5-byte one.

The reverse direction fails quietly, and clients should know it: a **v5 client
talking to a v6 device gets no error**. Its content writes are dropped (no
session), its Session characteristic does not exist to it, and the screen simply
never changes. There is no notification, because there is no session to notify.
Version-check first; it is the only signal there is.

Screen pixel dimensions (bytes 17..20) are orientation-corrected, i.e. exactly
the pixel canvas an image push should target.

---

## Status characteristic — setting one tag without re-pushing content

Phone → device. Three bytes:

```
byte 0:  sessionId
byte 1:  tagId       (one the peer declared)
byte 2:  state       (0 hidden, 1 outline, 2 filled)
```

The state-only path. Use it when a tag changes but the content did not — the
user saved the article that is already on screen, playback started, a sync
finished. It avoids re-sending a body to flip one chip, and redraws with a
no-flash differential refresh.

Use the content field `0x07` instead whenever the content is changing too, so
the two commit together.

Writes from a non-foreground session are ignored, and a `tagId` the peer never
declared is ignored.

## On-screen behaviour

Not wire format, but client-visible, and decided here so apps can rely on it:

- **Foreground app disconnects** — the device **holds the last content on
  screen**, then falls through to the sleep/icon screen on the existing idle
  timeout (5 minutes). It does not blank on disconnect. A photo stays a photo;
  an article stays readable after the phone walks away.
- **No peer has ever paired** — "Waiting for phone".
- **Paired peers exist, none connected, idle timeout elapsed** — the icon grid.
- **A session holds the foreground but has pushed nothing** — "Waiting for
  `<name>`", using that peer's display name.

---

## Storage layout (device side)

Informational — clients never see these paths, but they explain what "per-peer"
means:

```
/.crosspoint/companion/
  peers.json                 index: peerKey -> { appId, installId, displayName, lastSeenMs }
  peers/<peerKey>/
    token.bin                16-byte pairing token
    icon.bin                 1-bpp sleep-screen icon
    ui.bin                   UI declaration (button routing/labels + tag labels)
    data/                    per-peer scratch: staged image, event logs
```

Assets are stored as the exact bytes the phone pushed, digest included — the
digest has to survive verbatim anyway, and a blob costs no parser. `peerKey` is
the first 8 hex chars of a SHA-256 over `appId || installId`. Peer
directories are capped at 32, evicting the least recently seen — unbounded
growth would make `peers.json` unbounded, and it is parsed into RAM.

---

## Version history

### v6 changes from v5 — **breaking**

Migration checklist for a v5 client:

1. **Subscribe to the Session characteristic and complete a handshake before
   anything else.** Content pushed without a foreground session is dropped
   silently. This is the whole break.
2. **Add `sessionId` to every Content frame** — START byte 2, CHUNK byte 1, END
   byte 1.
3. **START's length field widened from uint16 to uint32** (bytes 3..6). START is
   now 7 bytes, not 4.
4. **Button-event payload gained a leading `sessionId` byte**; header and
   duration shifted by one, content-id now starts at byte 4.
5. **The Status characteristic sets one app-declared tag**, 3 bytes
   (`sessionId`, `tagId`, `state`), not a 1-byte `READ_LATER_SAVED`. The device
   holds no vocabulary: declare your tags in field `0x05`, then switch them.
   Tags do not auto-clear on a body push — push field `0x07` in the same atomic
   batch instead.
6. **Capability characteristic grew from 5 bytes to 23** with a new layout past
   byte 4.
7. **Push a UI declaration (field `0x05`) before `ACQUIRE`.** There is no
   default; without one `ACQUIRE` is denied and nothing renders. It carries both
   button routing/labels and tag labels.
8. Optional but recommended: push an icon (field `0x06`) so the app appears on
   the sleep screen.
9. New fields available: `0x04` image, `0x05` UI declaration, `0x06` icon,
   `0x07` tag state.
10. `deviceId` and screen pixel dimensions are now readable from the capability
    characteristic, which is guaranteed readable *before* the handshake — check
    byte 0 and refuse a non-6 device with a real message, because a v6 device
    ignores a v5 client silently.
11. `ACQUIRE` is asynchronous — wait for `FOREGROUND`, and re-push everything on
    every `FOREGROUND`, not just the first.

Design decisions made during implementation, beyond
`docs/companion-multi-app-design.md` §9: the `helloTag` correlation field, the
`ACQUIRE_DENIED` / `ASSET_ACK` / `IMAGE_STATUS` notifications, `sessionId` on
button events and Status writes, the uint32 START length, the capability block's
screen-pixel and content-id-cap entries, and the replacement of the named
`READ_LATER_SAVED` status byte with app-declared tags. Each is recorded in place
above with its rationale; §12 of the design doc records the resolutions of its
open questions.

### v5 changes from v4

Replaced the button-event characteristic's single semantic event byte
(PLAY_PAUSE/PREV/NEXT/READ_LATER) with a packed raw-button-id + hold-duration
payload, repeated while a button is held. Removed the `kSideUpMeansPrev`
UP/DOWN remapping — raw button identity is reported and the client owns the
interpretation.

### v4 changes from v3

Added the `0x80` final-field flag on the Content characteristic's START field
byte for atomic multi-field pushes.

### v3 changes from v2

Added the content-id content field (`0x03`) and the content-id bytes appended
to every button-event notification.

### v2 changes from v1

Replaced the button-event byte values (CONFIRM/BACK → PLAY_PAUSE/PREV/NEXT/
READ_LATER) and added the Status characteristic.

---

## Reference implementation status

- **Firmware**: `src/CompanionBle.h`/`src/CompanionBle.cpp` (service and
  characteristics, session table, content reassembly, image streaming to SD,
  button-event notify, capability characteristic);
  `src/CompanionPeerStore.{h,cpp}` (peer directory, tokens, asset digests, UI
  declaration, icons); `src/activities/companion/CompanionModeActivity.{h,cpp}` (the
  on-device screen: pagination, pairing prompt, button routing, image render,
  sleep grid). See `docs/companion-mode-implementation-notes.md` for the
  bring-up log.
- **Swift client**: `clients/swift/CompanionKit` — a SwiftPM package
  implementing discovery, the handshake, token/appId/installId persistence,
  ACQUIRE/RELEASE, asset digest compare-and-push, the framer, and button-event
  decoding. Shared by this fork's consumer apps; see its `README.md`.
- **Python client**: `scripts/push_companion_content.py` pushes title/body/
  content-id, a UI declaration, an icon and an image over BLE straight from a dev
  machine (`bleak`, see `scripts/requirements.txt`). Run with `--help`. It
  implements the exact framing above and is the fastest way to exercise the
  firmware without an app.
- No firmware-side automated tests exist — this project has no on-target test
  harness. CompanionKit's framer and handshake codec have `swift test` unit
  tests; everything on-device is verified by the checklist below.

## Manual verification checklist (on hardware)

Sessions and pairing:

1. Fresh device (or with `/.crosspoint/companion/` deleted): connect, send
   `HELLO` with no token. Confirm the pairing prompt appears with the app's
   display name and that `HELLO_PENDING` arrives.
2. Press CONFIRM: `HELLO_OK` arrives with a non-zero `sessionId`, a 16-byte
   token, and all-zero asset tags.
3. Disconnect, reconnect, `HELLO` with the stored token: `HELLO_OK` immediately,
   no prompt on screen.
4. `HELLO` with a corrupted token: prompt appears again; press BACK; confirm
   `HELLO_DENIED(USER_REJECTED)`.
5. Leave the prompt untouched for 30 s: `HELLO_DENIED(TIMEOUT)`.
6. Send `HELLO` five times with distinct `installId`s on one link: the fifth
   gets `HELLO_DENIED(NO_SESSION_SLOTS)`.
7. Two `HELLO`s in flight at once with different `helloTag`s: each reply
   carries the matching tag.

UI declaration gating:

8. Immediately after `HELLO_OK` with an all-zero declaration digest, send
   `ACQUIRE`: confirm `ACQUIRE_DENIED(NO_UI_DECLARATION)` and nothing on screen
   changes.
9. Push field `0x05` with a valid declaration: `ASSET_ACK(STORED)`. `ACQUIRE`
   now returns `FOREGROUND`.
10. Reconnect: `HELLO_OK`'s digest block reports the digest just pushed, not
    zeros. Push nothing and `ACQUIRE` — accepted.
11. Push a declaration with a new digest and different labels: the hint row
    changes, and `CMD:CUI` reads back both the buttons and the tags.
12. Map a button to `LOCAL_PAGE_NEXT` and confirm it pages on-device with no BLE
    event; map the same button to `REMOTE` and confirm it now notifies instead.
13. Map POWER to `NONE` and confirm POWER still sleeps the device.

Content and sessions:

14. Push a multi-page body, page with the mapped local-paging buttons, confirm
    page-turn hints appear only when that direction is pageable.
15. Push a long title and confirm it wraps onto up to 2 lines instead of eliding.
16. Push title + body + final-flagged content-id and confirm both change in one
    redraw, not the headline first.
17. Press a mapped `REMOTE` button and confirm the event carries the right
    `sessionId` and the pushed content-id.
18. Push a *different* content-id and confirm a stale client-side comparison
    would reject the next press.
19. Declare two tags in the UI declaration, then write Status
    `[sessionId, <tagId>, 2]` and confirm that tag's chip appears filled with no
    flash; `1` for outline, `0` to hide. Confirm an undeclared id does nothing.
20. Push a new body afterwards and confirm the tag does **not** reset itself,
    then push title+body+`0x07 | 0x80` together and confirm the content and the
    tag change in one redraw.
21. Confirm a long tag label is truncated rather than crowding the title, and
    that hiding every tag returns the title to full width.
20. With two sessions live, push content from the background session: nothing on
    screen changes. `ACQUIRE` from it: the other session gets
    `BACKGROUND(PREEMPTED)` and the screen clears to the new session's content.
21. `RELEASE` from the foreground: `BACKGROUND(RELEASED)`.
22. Send `BYE` and confirm a later frame with that `sessionId` is ignored.

Images:

24. Push a correctly-sized grayscale PNG using only `{0,85,170,255}`:
    confirm it renders full-screen, the grayscale settle runs, and
    `IMAGE_STATUS(DISPLAYED)` arrives.
24. Confirm the staged file lands under `peers/<peerKey>/data/` and that free
    heap during the transfer stays near its idle value (nothing image-sized was
    allocated).
25. Push garbage bytes as field `0x04`: `IMAGE_STATUS(DECODE_FAILED)` and the
    previous screen is retained.
26. Disconnect mid-image: confirm the partial staged file is discarded and the
    device does not try to decode it.
27. Push an image larger than the advertised max: `IMAGE_STATUS(REJECTED_SIZE)`.
28. Push a body after an image and confirm the screen returns to text.
28b. With a tag visible, push an image and confirm the chip is drawn over the
    print; hide every tag, re-push, and confirm the print is untouched.

Icons and sleep screen:

29. Push a 512-byte icon: `ASSET_ACK(STORED)`, tag reported on next connect.
30. Push an icon of the wrong length: `ASSET_ACK(REJECTED_SIZE)`.
31. Pair two apps and idle past the timeout: both icons appear on the sleep
    grid, one tile per `appId`.
32. Pair the same `appId` from two `installId`s: still one tile.

Power and recovery:

33. Disconnect with content on screen: content is retained, not blanked.
34. Leave it idle past 5 minutes with no connection: the device sleeps.
35. Disconnect/reconnect while content is loaded and confirm the re-push lands
    (not stuck on a waiting screen).
36. Confirm free heap after a full pair → push → image → disconnect cycle
    returns to its pre-session value (no leak across sessions).
