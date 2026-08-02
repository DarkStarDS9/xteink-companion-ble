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

**v9 — the current contract.** v6 was a **clean break**: the session handshake
is mandatory, and a client that pushes content without a valid session is
ignored. A v5 client will connect, push, and see nothing happen. v9 keeps that
shape unchanged and makes one further breaking change on top of it: the image
field's `CHUNK` gains a sequence number and is now pushed over Write Without
Response — see "v9 changes from v8" below.

This document is **authoritative** and is written first on purpose: consumer
apps are built against it while the firmware side lands. Where the firmware and
this document disagree, the firmware is wrong.

> ### ⚠ Nothing in v6 has ever executed on the wire
>
> As of 2026-07-28, the firmware implements **all** of v6 and **none of it has
> been exercised by a BLE client.** Not one handshake, content push, image, icon
> or tag has crossed the link. The device boots, advertises and answers serial
> commands; that is the whole of what has been confirmed.
>
> BLE itself works fine on this development machine — the gap is that the
> automated test harness that would exercise the checklist below has not been
> run yet. See "Manual verification checklist" at the end of this document —
> 36 items, all open — and `docs/companion-test-console.md` for how to run
> them.
>
> **`docs/companion-mode-implementation-notes.md` § "v6 bring-up log" is the
> single authoritative answer** to what has and has not been proven, with the
> risks ranked and the measured budgets.
>
> **Treat every behaviour described here as specified-and-implemented, not
> proven.** Highest-risk unproven areas: SD access from the NimBLE host task
> during pairing and image staging (4 KB stack, a crash would look like a
> pairing failure), the entire image decode and grayscale-settle path (which
> already yielded one real bug found by reading rather than running — see
> `renderImage()`'s black/white base), and everything that draws: icon grid,
> tag chips, pairing prompt.

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

## Design rule: a display string is never an identifier

Every user-visible string in this protocol — a tag label, a button label, a
peer's display name — is **presentation only**. Nothing keys on it, compares it,
sorts by it, deduplicates on it, or derives a path or a directory name from it.
Identity is always a separate opaque value: a `tagId`, a `buttonId`, an `appId`
and `installId`.

This is not a style preference; it is what makes localization survivable.
Display strings change under a running app — the user switches phone language,
a server-driven label set updates with no app release, an app is renamed in a
new version. If any of those changed identity, then a user switching to German
would find their tags replaced by different ones, their saved article unsaved,
or their paired app suddenly a stranger the device asks them to confirm again.

Concretely, in v6:

| String | Never used for | Identity is |
|---|---|---|
| Tag label | tag state, matching, ordering | `tagId` from the UI declaration |
| Button label | routing, hint placement, matching | `buttonId` (mirrors the HAL) |
| Peer display name | `peerKey`, directory names, index lookup | `appId` + `installId` |

Labels *are* part of an asset's content, so changing one changes its digest and
the asset is re-pushed. That is the digest doing its job — deriving a version
from content — and is the opposite of deriving identity from a label.

**Any field added to this protocol inherits this rule.** If a new string ever
needs to be matched on, that is the signal it should have had an id.

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

- Only the foreground session may push **content** and receive button events.
  Content frames tagged with a background (or unknown) session id are
  **silently dropped**. Assets (`0x05`, `0x06`) are the exception and are
  accepted from any live session — see "Assets do not require the foreground"
  below.
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

`HELLO` (up to 101 bytes) and `HELLO_OK` (up to 30 bytes) exceed BLE's minimum
MTU, so a handshake needs an ATT MTU of at least 105. The device requests 185
and both iOS and Android negotiate well above the floor in practice; a central
that cannot get past 23 cannot use v8 at all.

### Phone → device (write)

```
0x01 HELLO      helloTag[2]  appId[16]  installId[16]
                tokenLen:1  token[tokenLen]         (tokenLen 0 or 16)
                nameLen:1   name[nameLen]           (UTF-8, <= 24 bytes, may be empty)
                userNameLen:1  userName[userNameLen] (UTF-8, <= 24 bytes, may be empty)
0x02 BYE        sessionId
0x03 ACQUIRE    sessionId
0x04 RELEASE    sessionId
```

`name` is the display name shown on the pairing prompt and the "waiting for
<app>" screen — the app's user-visible name ("Snap2Ink"), not the peer's.
Longer names are truncated to 24 bytes on a UTF-8 boundary.

`userName` is a separate, user-facing label for *this install* — e.g. which of
the user's own devices/accounts this is — distinct from `name`, the app's own
name. It exists so two installs of the same app (an iPhone and an iPad, say)
don't look identical on the gallery picker (see "On-screen behaviour" below):
same icon, same app name, but a different `userName` underneath. Resent on
every `HELLO`, including reconnects, so it stays current with no digest or
re-pairing needed — the same "just resend it" treatment as `name`. Same 24-byte
UTF-8-boundary truncation. May be empty, in which case the picker falls back to
the peer's `name`.

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
0x89 IMAGE_CHUNK_ACK sessionId seq:2                          -- new in v9
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
                         0x01 DECODE_FAILED     wrong byte count for a raw 2bpp full-screen image
                         0x02 REJECTED_SIZE     exceeded max image length
                         0x03 STORAGE_FAILED    could not stage to SD
                         0x04 SEQUENCE_GAP      a CHUNK's sequence number skipped ahead of what
                                                was expected -- new in v9, see "Image field"
```

`IMAGE_CHUNK_ACK` is a progress marker only, sent roughly every 32 CHUNKs
during an image push (see "Image field" below) — it is not required for
correctness and a client that ignores it loses nothing but early failure
detection. `seq` is the highest contiguous CHUNK sequence number the device
has processed.

`ASSET_ACK` and `IMAGE_STATUS` are the two things a client genuinely cannot
work out for itself: whether the device stored the asset, and whether the
image decoded. Everything else about rendering is deterministic from what was
pushed.

`IMAGE_STATUS` is **strictly a response to an image push**, never a broadcast
about what is on screen. Exactly one arrives per pushed field `0x04`, and none
at all for a redraw the client did not cause — connecting while an older image
is still displayed, a foreground handover, or the user paging the device's
local gallery all repaint the panel silently. A client may therefore treat the
first `IMAGE_STATUS` after starting a push as that push's answer.

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

**Assets may be re-pushed at any time during a session, not only at
enrollment.** The digest block in `HELLO_OK` is where reconciliation usually
starts, but nothing restricts a push to that moment: send a new UI declaration
mid-session and the device stores it and redraws the hints and tag labels
immediately. That matters because labels are localized — a user switching their
phone to German should see German labels without reconnecting, and a
server-driven label set can change with no app release. Do not build a one-shot
declare-at-enrollment path.

A re-pushed declaration changes **presentation only**. Tag state and button
routing survive by id; nothing the user did is lost because the words changed.

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

### Framing (v6, `CHUNK` amended in v9 for the image field)

```
START:  byte 0      opcode = 0x01
        byte 1      field | final-flag   (low 7 bits = field id; 0x80 = "final field of this push")
        byte 2      sessionId
        bytes 3..6  total payload length, uint32 little-endian

CHUNK:  byte 0      opcode = 0x02
        byte 1      sessionId
        bytes 2..N  payload bytes                    (every field except image)

CHUNK:  byte 0      opcode = 0x02
        byte 1      sessionId
        bytes 2..3  sequence number, uint16 little-endian, starting at 0
        bytes 4..N  payload bytes                    (image field only, v9+ — see "Image field")

END:    byte 0      opcode = 0x03
        byte 1      sessionId
```

A push of one field is: one `START` declaring the field and its total byte
length, one or more `CHUNK`s carrying the bytes in order (each sized to the
negotiated MTU minus the CHUNK framing overhead — 2 bytes, or 4 for the image
field), then one `END`. Fields are independent pushes over the same
characteristic — send one field's full START/CHUNK…/END before starting the
next. The device does not assume an order beyond "each field is internally
ordered".

**`sessionId` is on every frame, including `CHUNK`.** It costs one byte per
packet and buys the guarantee that a stray write from a background app can
never inject bytes into another app's in-flight transfer. The apps on this
protocol are cooperating, not adversarial — but they are written by different
codebases on different release cycles, which is the same failure mode.

**Assets do not require the foreground; content does.**

- Fields `0x05` (UI declaration) and `0x06` (icon) are accepted from **any live
  session**. They are per-peer state, not screen content — a background app may
  refresh its labels or its icon without taking the screen. This is also
  required rather than convenient: `ACQUIRE` is refused until a UI declaration
  is stored, so if pushing one needed the foreground, a peer could never push
  the declaration that would let it become foreground. Enrollment would
  deadlock.
- Fields `0x01` `0x02` `0x03` `0x04` `0x07` (title, body, content-id, image, tag
  state) are dropped unless the sending session currently holds the screen.

Frames from an unknown session are always dropped. Pushing a UI declaration
while another app holds the screen stores it silently; it takes effect for you
when you next acquire.

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
Response") for the CHUNK packets so BLE's own link-layer ack applies.

**Except the image field, where the recommendation is the opposite as of v9:
push its CHUNKs with Write Without Response.** Measured on real hardware
(ESP32-C3, 15ms connection interval, 2M PHY): a write-with-response round trip
costs ~120ms regardless of connection interval — the peripheral's own handling
is 0-1ms, so the cost is the ATT round trip itself, not anything the firmware
does. That floors a ~200-chunk image transfer at several times the link's real
throughput. Write Without Response has no such round trip, but drops
CoreBluetooth/BlueZ's own delivery guarantee, which is why the image field's
CHUNK carries the sequence number described above: a dropped or reordered
chunk is now something the device *detects* (`IMAGE_STATUS(SEQUENCE_GAP)`)
instead of something that silently corrupts the reassembled 2bpp payload. Keep
`START` and `END` on Write, so the phone still gets a reliable begin/end ack.
Every other field is unaffected — small enough that the per-chunk round trip
this exists to avoid barely matters, and (having no sequence number) still
depends on Write's link-layer ack for correctness.

A client pushing over Write Without Response should throttle to what the OS
buffers for un-acked WWR writes (iOS: `CBPeripheral.canSendWriteWithoutResponse`
/ `peripheralIsReady(toSendWriteWithoutResponse:)`) rather than writing in a
tight loop — `IMAGE_CHUNK_ACK` (above) is a diagnostic on top of that, not a
substitute for it, since it arrives only every ~32 chunks and says nothing
about how many writes the OS will currently accept.

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

### Image field (`0x04`) — raw packed 2bpp, full screen, no header

The client captures/crops a photo, **dithers it itself** to the panel's
four-level palette, packs it into this field's raw wire format (below), and
pushes it as field `0x04`. The device streams the bytes straight to
`peers/<peerKey>/data/incoming.raw` on the SD card as they arrive — it never
holds the image in RAM — and on `END` unpacks it directly to the framebuffer
(no decompression, no gray-level math: each 2-bit sample already *is* the
final display level 0–3), then runs the two-pass grayscale settle.
`IMAGE_STATUS` reports the outcome.

**The device keeps more than the last push.** Once a transfer completes,
`incoming.raw` is moved into a bounded per-peer gallery (`images/`, up to
`kMaxImagesPerPeer` = 6 entries, oldest overwritten once full — see
`CompanionPeerStore.h`) instead of being left as a single scratch file the
next push would silently clobber. The user can page back and forth through a
peer's own gallery with `UP`/`DOWN` while a photo is on screen — see the
`UP`/`DOWN` note under "UI declaration field" above. This is entirely
device-local: there is no wire opcode for it, no capability bit, and the app
is never told navigation happened, so this paragraph is the only thing an app
author needs to read about it.

This replaced an earlier PNG-based format (protocol v6 and before). PNG
decoding needs PNGdec's ~44 KB working set (decoder object + inflate window)
plus a 16 KB safety margin — but measured free heap with one BLE peer
connected is only ~47–50 KB on this part, below that floor. Every PNG push
failed with "not enough heap for PNG decoder"; it was a hard wall, not a flaky
threshold. This format's entire device-side working set is one packed row
(a few hundred bytes), so it has no such floor. See "v7 changes from v6" in
the version history for the full rationale, and
`lib/Epub/Epub/converters/RawBitmapToFramebufferConverter.{h,cpp}` for the
device-side decoder.

The transfer plus the grayscale settle still takes a few seconds (the settle
re-decodes twice more by design). That is expected and is not optimised for; a
slow "developing" draw is thematically wanted, and this format made the actual
decode step itself effectively free — the settle's cost is now almost entirely
the e-ink refresh, not decoding.

**Wire format.**

- **The field's `CHUNK` carries a 2-byte sequence number (v9+).** See "Atomic
  multi-field pushes" above for why (bulk image CHUNKs go out over Write
  Without Response) and "Framing" for the exact byte layout. The device
  tracks the next expected value and fails the transfer with
  `IMAGE_STATUS(SEQUENCE_GAP)` the instant a CHUNK arrives out of order — a
  lost or reordered packet is detected rather than silently corrupting the
  reassembled payload the way it would with no sequence number. There is no
  partial-resume protocol: recovering from `SEQUENCE_GAP` means re-pushing
  the field from a fresh `START`.
- No header (beyond the CHUNK sequence number above). The payload is exactly
  `bytesPerRow * screenHeightPx` bytes —
  nothing else. Both sides already know the dimensions from the capability
  characteristic (bytes 17..20), so a length/width/height header would be
  redundant weight on every single push.
- `bytesPerRow = ceil(screenWidthPx / 4)` — 4 pixels per byte, each row padded
  out to a whole byte (so a new row always starts at a byte boundary; there is
  no bit-level carry between rows).
- Each pixel is a 2-bit sample, value `0..3`, packed **MSB-first**: within a
  byte, pixel 0 (the leftmost of the 4 it covers) occupies bits 7–6, the next
  pixel bits 5–4, then bits 3–2, then bits 1–0 for the rightmost. This matches
  the packing this codebase already uses on-disk for its pixel cache (see
  `PixelCache.h`) — not a new scheme.
- Pixel value meaning: `0` = black, `3` = white, `1`/`2` = the two mid gray
  levels — the same 4-level scale the old PNG path's `gray / 85` bucketing
  produced. Because the client already dithers to this exact palette before
  encoding, no further gray-level math happens on decode; the 2-bit sample
  *is* the display level.
- Rows are stored top to bottom, left to right, in the same orientation-corrected
  logical coordinate space `getScreenWidth()`/`getScreenHeight()` (and the
  capability characteristic's pixel-dimension bytes) already use — the same
  space a client already targets for centering text.

**Dimensions are exact, not "up to."** The payload must be **exactly**
`screenWidthPx x screenHeightPx` as read from the capability characteristic
(bytes 17..20) — **do not assume a panel size**; a measured X3 in Companion
Mode reports **528 x 792**, not the 800 x 480 that older notes in this repo
assume. Unlike the old PNG path, there is **no scaling and no centering**: a
payload of the wrong byte count is rejected outright (`IMAGE_STATUS`
`DECODE_FAILED`) rather than resampled or cropped. Cropping, scaling and
rotation are entirely the phone's job; the device never does any of the
three for an image push.

For this device's measured 528 x 792 panel: `bytesPerRow = ceil(528/4) = 132`,
so every push is exactly `132 * 792 = 104544` bytes — fixed, with no
compression-dependent variance and no worst-case blowup risk.

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

**A tag change alone does not re-develop the print.** Setting a tag while an
image is displayed redraws only the chips, over the retained image, with a
differential refresh. It does not re-decode or re-settle — that would cost
seconds for a mark that moved.

**On timing, if you want a "finished developing" mark.** A tag pushed *with* the
image is drawn when the image is drawn, which is the *start* of the grayscale
settle, not the end. If you want a mark that means "this print has finished
resolving", set it with a standalone Status write after `IMAGE_STATUS(DISPLAYED)`
arrives — that notification is sent after the settle completes, and the redraw it
triggers is the cheap chips-only one described above. The device will not infer
this for you: when a tag means "done" is your semantics, and a firmware that
filled a mark because it decided that is what the app meant would be
interpreting.

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

byte         tag render style           <- optional; absent means BORDERED
byte         capabilities bitmask       <- optional; absent means none set
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
- **`UP`/`DOWN` left as `NONE` (or `LOCAL_PAGE_PREV`/`LOCAL_PAGE_NEXT`) are used
  by the device for image gallery navigation** while a pushed photo (field
  `0x04`) is on screen: the device keeps the last several images an app has
  pushed (see "Image field" above) and lets the user page back and forth
  through them locally with the side buttons. `LEFT`/`RIGHT` were deliberately
  not used for this: they're the buttons most apps already route to
  `LOCAL_PAGE_PREV`/`LOCAL_PAGE_NEXT` for paging text, so `UP`/`DOWN` are the
  pair actually free in practice. This is not a wire behaviour — no
  notification is sent, no capability bit exists for it — so it costs an app
  nothing to be unaware of it. An app that declares its own routing for
  `UP`/`DOWN` (e.g. `REMOTE`, for something like camera control) is never
  overridden; the gallery only engages on whichever of those two buttons the
  app's own map leaves unclaimed (or routes to local paging, which is already
  a no-op outside text content).

#### Tags

A tag is a short labelled chip the device draws beside the title, which the app
switches on and off at runtime. **The firmware defines no tags.** There is no
built-in "saved", no fixed slots, no reserved ids, no enum of permitted values —
the app declares which tags exist and what each is called, exactly as it declares
button labels, and the device stores the strings and renders them. This is the
same three-part shape as buttons: the app declares the set, the device persists
and draws it, and the runtime message carries only state.

- **`tagId` is identity; the label is only ever drawn.** State updates reference
  the id and never the text. This separation is load-bearing, not incidental:
  labels are localized, so they change when the user switches phone language and
  can change from a server update with no app release at all. A design where
  state referenced label text would decide that switching to German had replaced
  every tag with a different one, and an article saved in English would come back
  unsaved in German. Change labels freely; identity and state are untouched.
- **`tagId` is per-peer.** It means whatever the declaring app says it means, and
  two apps both using id `0` never collide, because each peer's declaration is
  its own. Ids need not be contiguous or start at zero.
- **Labels are text, not emoji.** The device draws with a Latin font; an emoji
  renders as tofu. Send the word and keep the glyph for your own surfaces.
- Limits: **6 tags**, label **24 bytes** each — the same cap as the display name,
  sized for localized labels rather than English ones ("Später lesen" is 13 bytes
  before you start). Longer labels are truncated on a UTF-8 boundary; tags past
  the sixth are dropped. These bound the device's per-peer RAM (162 bytes,
  resident only for the foreground peer).
- Every tag starts **hidden** when the declaration is loaded. Declaring a tag
  says it exists, not that it is on.
- A tag id that was never declared is ignored wherever it appears. The
  declaration is the only place tags come into existence.

**Tag render style.** One more byte after the tag list, choosing how the whole
row looks — a presentation choice, not per-tag state:

```
0x00  BORDERED  (default)  OUTLINE draws a box; FILLED draws a filled box with
                            knocked-out (inverted) label text. The original look.
0x01  PLAIN                OUTLINE draws nothing at all, same as HIDDEN; FILLED
                            draws the label as plain text, no box.
```

Optional and trailing, so it costs nothing for a client that doesn't care: omit
it (end the declaration after the tag list, as before) and the device treats
that peer as `BORDERED`. An out-of-range byte is treated the same as absent.
This is a per-peer, whole-row choice — there is no way to mix styles within one
app's tag row.

**Capabilities.** One more optional trailing byte, after the tag render style
byte, declaring what this peer's app can do beyond title/body:

```
bit0  IMAGE_GALLERY   this app pushes photos (field 0x04) and wants a tile in
                       the on-device gallery picker — see "On-screen
                       behaviour" below. Every other bit is reserved.
```

Same "ran out of buffer" absence convention as the style byte, and the two are
positional, not tagged: a declaration that wants capabilities but not a custom
style must still send the (default) style byte first. Every client in this
repo emits both bytes unconditionally for exactly this reason — there's no
reason to omit either once you're sending one.

This is deliberately a capability, not something the device infers from
whether a peer happens to have pushed an image before: an app that supports
photos but hasn't pushed one yet (e.g. just after pairing) still belongs in
the picker, so the user can see that it has no photos yet rather than the app
being invisible until its first push.

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
the device. **23 bytes**, unchanged in layout since v6 — v7, v8 and v9 each
only bumped the version number itself (byte 0), for field `0x04`'s payload
format change, the `HELLO`/UI-declaration additions, and the image `CHUNK`
sequence number respectively; see "v7 changes from v6", "v8 changes from v7"
and "v9 changes from v8":

```
byte 0        protocol version = 9
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
whole reason to guarantee it. A v9 client should:

1. Read the characteristic immediately after connecting.
2. Check byte 0. If it is not 9, tell the user *"this reader's firmware is too
   old for this version of <app>"* (or too new) and stop. Do not attempt the
   handshake, and do not guess at the layout — the 23-byte value shares nothing
   past byte 4 with v5's 5-byte one.

The reverse direction fails quietly, and clients should know it: a **v5 client
talking to a v6-or-later device gets no error**. Its content writes are dropped (no
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
  an article stays readable after the phone walks away. **Exception:** if the
  disconnecting peer declared the `IMAGE_GALLERY` capability (see "UI
  declaration field" above) and never pushed an image during that session, the
  device shows that peer's own stored gallery immediately instead of waiting
  out the idle timeout — an empty "waiting" screen or stale prior content is
  less useful than photos the app already pushed in an earlier session. If it
  has no stored images either, a brief "No images yet" message is shown before
  falling through to the icon grid as usual.
- **No peer has ever paired** — "Waiting for phone".
- **Paired peers exist, none connected, idle timeout elapsed** — the icon grid.
- **A session holds the foreground but has pushed nothing** — "Waiting for
  `<name>`", using that peer's display name.
- **Gallery picker** — pressing CONFIRM on the icon grid enters an interactive
  picker over every enrolled peer that declared `IMAGE_GALLERY`, most recently
  seen first (not grouped by `appId`, unlike the icon grid: two installs of
  the same app have two separate galleries and stay two separate tiles, each
  labelled with `userName` — see "Phone → device (write)" above — falling
  back to `name`). UP/DOWN move the cursor, CONFIRM loads the highlighted
  peer's stored gallery (a local SD read — the peer need not be currently
  connected), BACK returns to the icon grid. If no peer has declared the
  capability, CONFIRM on the icon grid shows a brief "No photo apps paired
  yet" message instead of entering an empty picker. If the selected peer has
  no stored images, a brief "No images yet" message is shown and the picker
  stays up. None of this involves the phone or any wire message — it is
  firmware-local browsing of already-pushed photos, the same "dumb firmware"
  local browsing as the existing UP/DOWN image-gallery navigation described
  under "UI declaration field" above, just entered a different way. It is
  *not* a launcher: the device cannot and does not start anything on the
  phone.

### Deep sleep and boot

Deep sleep can be reached three ways: the idle timeout above, an app-mapped
button routed `LOCAL_SLEEP`, or the device's own power button. All three
agree on what the panel shows going into sleep:

- **An image is on screen** — the photo is left untouched; the device adds a
  small crescent indicator in the bottom-left corner (the same corner text
  mode's battery percentage occupies) and sleeps. Nothing else changes, so a
  photo stays recognisable through sleep, not replaced by a generic screen.
- **Anything else on screen** (icon grid, waiting text, an open text
  print) — the icon grid (or waiting text, if no peer has an icon yet) is
  drawn **inverted**, with "Sleeping" underneath, and the device sleeps on
  that frame.
- **Waking** always repaints once, immediately, straight to the plain idle
  screen (the icon grid, or waiting text) — not upstream CrossPoint's
  splash/logo, and with no "booting" label either: BLE is already up and
  advertising by the time this first paint happens, so there is nothing left
  to report as still in progress.

---

## Storage layout (device side)

Informational — clients never see these paths, but they explain what "per-peer"
means:

```
/.crosspoint/companion/
  peers.json                 index: peerKey -> { appId, installId, displayName, userName, lastSeenMs }
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

### v9 changes from v8 — **breaking**

1. **The image field's (`0x04`) `CHUNK` gains a 2-byte little-endian sequence
   number**, right after `sessionId` — see "Framing" and "Image field" above.
   Every other field's `CHUNK` is unchanged.
2. **The image field's `CHUNK`s are now pushed over Write Without Response**
   (recommended, not enforced at the GATT level — the Content characteristic
   already advertised both write types since v6). `START` and `END` stay
   Write. See "Atomic multi-field pushes" above for the measurement behind
   this and why the sequence number exists.
3. **New device → phone notification `IMAGE_CHUNK_ACK` (`0x89`)**, sent every
   ~32 CHUNKs during an image push — see "Session characteristic". Diagnostic
   only; ignoring it costs a client nothing but early failure detection.
4. **New `IMAGE_STATUS` result `SEQUENCE_GAP` (`0x04`)** — the device
   detected a CHUNK sequence number that skipped ahead of what it expected.
5. **Capability byte 0 bumped from 8 to 9.** No other capability bytes moved.

Why: measured on real hardware (ESP32-C3, 15ms connection interval, 2M PHY),
a write-with-response round trip on the image CHUNK path costs ~120ms
regardless of connection interval, floored by the ATT round trip itself
rather than anything the firmware does with the write (the peripheral's own
`onWrite` handling measured 0-1ms). For a ~200-chunk image push that is
several times slower than the link's real throughput. Write Without Response
removes that round trip, at the cost of BLE's own delivery guarantee — the
sequence number and the periodic ack together turn a silently corrupted
transfer into one the device (and, with `IMAGE_CHUNK_ACK`, the phone) detects
instead. There is no partial-resume protocol yet: recovering from a
`SEQUENCE_GAP` means re-pushing the field from a fresh `START`.

### v8 changes from v7 — **breaking**

1. **`HELLO` gains `userNameLen`/`userName`**, appended after `name`. See
   "Phone → device (write)" above. A v7 `HELLO` is one field short of what v8
   expects — this is why the bump is breaking rather than additive, even
   though it is the last field on the message.
2. **UI declaration gains an optional trailing capabilities byte**, after the
   tag render style byte — see "UI declaration field" above. Additive on its
   own (optional, absent-safe), bundled into this version bump because it
   ships alongside the `HELLO` change.
3. **Capability byte 0 bumped from 7 to 8.** No other capability bytes moved.
4. New on-device feature, no wire surface of its own beyond the two additions
   above: the gallery picker (see "On-screen behaviour").

Why: the gallery picker (an interactive grid over paired photo apps' stored
galleries — see "On-screen behaviour") needs a way to tell two installs of the
same app apart on screen, which `name` alone can't do since it's the app's own
name, identical across installs. `userName` is that per-install label.
Bundling the capabilities byte into the same bump avoided a third protocol
revision for two features that landed together.

### v7 changes from v6 — **breaking**

1. **Field `0x04` (image) is raw packed 2bpp, not PNG.** See "Image field
   (`0x04`)" above for the full wire-format spec. This replaces PNG entirely —
   there is no format-sniffing and no fallback to the old format.
2. **Capability byte 0 bumped from 6 to 7.** No other capability bytes moved;
   this bump exists solely so a client can tell the two image formats apart by
   version rather than by guessing at file content.

Why: PNG decoding needs PNGdec's ~44 KB working set (decoder + inflate
window) plus a 16 KB margin, but measured free heap with one BLE peer
connected is only ~47–50 KB on this part — below that floor. Every PNG image
push failed with "not enough heap for PNG decoder"; it was not a flaky
threshold; it could never succeed. Raw packed 2bpp needs one packed row of
scratch (well under 1 KB), so it has no such floor. Since v6 never executed on
real hardware (see the warning near the top of this document), there was no
shipped image behaviour to preserve, so the fix is a clean replacement of the
wire format rather than a heap workaround.

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

24. Push an exactly-sized raw packed 2bpp image (see "Image field (`0x04`)"
    for the byte layout) using only sample values `{0, 1, 2, 3}`: confirm it
    renders full-screen, the grayscale settle runs, and
    `IMAGE_STATUS(DISPLAYED)` arrives.
23b. **Bisect the image path with two encoders.** Snap2Ink ships a calibration
    target (eight bands answering "count the distinct greys", "is this band
    striped or flat", "is the border one pixel or two") that bypasses its own
    rasterizer and dither. Push that *and* a harness-generated raw payload of
    the same target (`scripts/companion_e2e_test.py`'s image-generation helper
    needs updating from PNG to raw packed 2bpp for this protocol version — see
    the wire format above). If only theirs is wrong the fault is phone-side;
    if both are wrong it is the firmware's decode or settle. Do this before
    debugging either side in isolation.
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
28c. **(v9)** Push an image over Write Without Response with correctly
    incrementing CHUNK sequence numbers: confirm `IMAGE_CHUNK_ACK` notifies
    roughly every 32 chunks and the transfer still ends in
    `IMAGE_STATUS(DISPLAYED)`. Then push one with a deliberately skipped
    sequence number: confirm `IMAGE_STATUS(SEQUENCE_GAP)` and that the
    previous screen is retained, the same as a decode failure.

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

Gallery picker (new in v8):

37. Pair an app that does not declare `IMAGE_GALLERY`: on the idle icon grid,
    press CONFIRM and confirm a brief "No photo apps paired yet" message
    appears instead of a picker grid.
38. Pair an app that declares `IMAGE_GALLERY` but has not pushed a `HELLO`
    with a `userName`: confirm its picker tile falls back to `name`.
39. Pair the same `IMAGE_GALLERY` app twice with two different `installId`s
    and two different `userName`s: confirm the icon grid still shows one tile
    (grouped by `appId`, unchanged), but the gallery picker shows two separate
    tiles, each labelled with its own `userName`.
40. With at least one `IMAGE_GALLERY` peer paired but never having pushed an
    image: enter the picker, select its tile, and confirm a brief "No images
    yet" message appears and the picker stays up (not the icon grid).
41. Push a few images to an `IMAGE_GALLERY` peer, disconnect, reconnect a
    different (or no) app so it's no longer foreground, then open the picker
    and select that peer: confirm its stored gallery loads and UP/DOWN page
    through it, matching the existing image-gallery navigation.
42. From a gallery reached through the picker, press BACK: confirm it returns
    to the picker grid (not the icon grid or a blank screen), with the cursor
    on the same tile as before.
43. While an `IMAGE_GALLERY` peer holds the foreground and has pushed at least
    one image this session, disconnect it: confirm the existing hold-last-
    content behaviour applies (no jump to its gallery — the "never pushed an
    image this session" exception does not apply here).
44. While an `IMAGE_GALLERY` peer holds the foreground and has pushed nothing
    this session, disconnect it: confirm the device jumps straight to that
    peer's stored gallery (if it has one) instead of showing "Waiting for
    phone" or holding stale content until the idle timeout.
45. Repeat the above with a peer that has no stored images at all: confirm a
    brief "No images yet" message, then the icon grid.
46. While browsing a gallery reached through the picker (peer not currently
    connected), have a *different*, currently-foreground app push new
    content: confirm the new content takes the screen immediately, same as it
    would over the plain icon grid.
