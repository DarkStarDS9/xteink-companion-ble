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

> **Versioning rule: never bump the protocol version unless explicitly told to.**
> Every client of this protocol is written by this project's author and they release in lockstep, so
> there is no deployed client to strand and backward compatibility is not a requirement. v12 is
> treated as **still in development**, not as a published contract: an additive change folds into
> v12's definition and this document is rewritten to describe v12 as always having included it,
> rather than earning a new version number. A capability feature bit inside the unchanged 23-byte
> layout is an acceptable discovery mechanism and does not by itself justify a bump. Everything the
> ToDo List functionality needed is v12. Resetting the version to **v1** before this is announced
> publicly is under consideration — don't do it unprompted, but don't design against it either.

**v12 — the current contract.** The shortest statement of it is: **a peer declares what kind of content it pushes, in its UI
declaration, and may push nothing else.** A mandatory content-shape byte
(`TEXT`/`IMAGE`/`LIST`) sits at offset 4 of field `0x05`, ahead of the button
count; a declaration without it is refused
(`ASSET_ACK(REJECTED_NO_SHAPE)`) and the peer never reaches the screen, and a
content field outside the declared shape is refused
(`RENDER_STATUS(REJECTED_SHAPE)`) exactly once per push. Every v11 client is
therefore refused until it declares a shape — accepted deliberately, since every
client of this protocol is written by this project's author. See "UI declaration
field" below and "v12 changes from v11".

v12 also carries the ToDo List check-off sync-back: the device announces that it
holds on-device edits (`LIST_STATE_AVAIL`) and the phone pulls them a window at
a time (`LIST_STATE_GET` → `LIST_STATE`), all on the Session characteristic and
with no new content field. See "List-state sync-back" below.

The v6-through-v11 contract underneath it is unchanged. v6 was a **clean break**: the session handshake
is mandatory, and a client that pushes content without a valid session is
ignored. A v5 client will connect, push, and see nothing happen. v10 kept that
shape unchanged and made one further breaking change on top of it: the
title/body fields' `CHUNK`s gain a sequence number and are now pushed over
Write Without Response, the same treatment the image field got in v9 — see
"v10 changes from v9" below. v11 widens `IMAGE_STATUS` (renamed
`RENDER_STATUS`) to also answer a text push, and correlates each answer to the
push that earned it with a `pushId` the *client* chooses and the device simply
echoes back — appended as `END`'s new required third byte and returned
verbatim as `RENDER_STATUS`'s trailing byte. (An earlier shape of this same
release answered with a `field` byte instead; it was replaced before any
consumer app had adopted it — see "Version history" for why a `field` byte
doesn't generalize the way `pushId` does.) See "Session characteristic" and
"Content characteristic" below, and "Version history" for why this is a
breaking change rather than an addition.

This document is **authoritative**: consumer apps are built against it, not
against whatever the firmware happens to do. Where the firmware and this
document disagree, the firmware is wrong. It is also written first by
convention — a wire change lands here in the same commit as the code that
implements it, never after.

> ### What has actually run on the wire
>
> **As of 2026-08-03, v11 is exercised end-to-end on real hardware.** This
> replaces a warning that stood here from 2026-07-28 to 2026-08-03 saying that
> nothing in v6 had ever crossed the link — that is no longer true, and the
> earlier caution should not be read into the current state.
>
> Proven by `scripts/companion_e2e_test.py` against a `[env:test]` build
> (66 assertions, passing, exit 0): enrollment from never-paired including the
> CONFIRM press, token reconnect, `ACQUIRE` gating, atomic multi-field content
> batches and their `RENDER_STATUS`, `pushId` correlation and the `pushId 0`
> silence rule, both sequence-gap paths, a batch discarded whole leaving the
> previous page intact, tags, preemption between two apps on one link, and a
> full-screen image push diffed pixel-for-pixel against `CMD:SCREENSHOT`. The
> device's own `[ERR]` log is scanned throughout, so a path that fails and
> silently falls back is caught rather than self-reported as success.
>
> Proven separately through `CompanionKit` (the client the consumer apps
> actually use), via `companion-bench`: image push throughput, text pushes with
> `awaitRender`, and the connection-profile ladder.
>
> **As of 2026-08-10, v12 has run on real hardware and passed.** A `[env:test]`
> build was flashed to a real X3 reader (`pio run -e test -t upload`,
> `/dev/cu.usbmodem212401`) and `scripts/companion_e2e_test.py` was run against
> it over BLE from a plain macOS terminal. Final run: **112 passed, 0 failed,
> 0 skipped**, exit 0, with the device's own `[ERR]` log scanned throughout and
> clean. The `[shape]` group — declared-content-shape enforcement, the change
> this section used to say was unproven — passed in that run, covering the
> shape-refusal paths (`RejectedShape`, `RejectedNoShape`) described below.
>
> One harness bug was found and fixed by this run (commit `097b2841`): the
> over-cap fixture for the list field originally built ~3400 single-character
> items, which cannot be encoded because item count is a u8 per group; it was
> rebuilt from 70 items of 250-byte text.
>
> An earlier run in the same session showed 2 failures, neither in `[shape]`'s
> refusal assertions or `[list]`: `"the previous screen is retained after a
> gapped image"` (`[image seqgap]`) and `"a refused text batch left the screen
> as it was"` (a screen-state sampling check inside `[shape]`). Both are
> screen-state *sampling* checks in an area with a prior commit titled
> "fix(harness): stop racing the shape group's own screen-state checks", and
> both passed cleanly on the immediately following identical run. Treat them as
> **flaky, not proven absent** — this document does not claim they are fixed,
> and does not claim the suite is reliably 112/112 run over run.
>
> **The ToDo List document field (`0x08`, LIST content shape) — Phase A, now
> hardware-verified.** `kFieldListDoc` completes the `LIST` shape v12 reserved
> but left with no content field (see "List document field" below); this is
> not a version bump, just v12's reservation finished. Implemented: the wire
> codec, ingest to `lists.bin` (the verbatim wire bytes, not a JSON re-encoding
> -- see the "Storage layout" section below), `Screen::List` rendering with
> paging/list-switching, and the offline icon-grid/picker entry point. The e2e
> harness's `[list]` group
> (`scripts/companion_e2e_test.py`) ran against the same real X3 on 2026-08-10
> and passed **17/17**, covering: a list doc pushed to a TEXT peer answered
> `RejectedShape`; the LIST peer's declaration stored and holding the screen; a
> well-formed multi-list document accepted and rendered; the device reporting
> the `list` screen; a title pushed to a LIST peer refused `RejectedShape`; tag
> state to a LIST peer refused `RejectedShape`; a truncated document refused
> and specifically `DecodeFailed`; a `checked=2` document refused; an over-cap
> (>16 KB) document refused and specifically `RejectedSize`; and no unexpected
> `[ERR]` log lines.
>
> **The list-state sync-back is hardware-verified.** The availability notify,
> the paginated pull and `list_state.bin` are implemented, host-tested
> (`test/companion_todo_diff/`) and exercised end-to-end against the same real
> X3: the full `scripts/companion_e2e_test.py` suite passed **159 passed, 0
> failed, 0 skipped** on three consecutive runs, with the device's own `[ERR]`
> log scanned and clean throughout.
>
> **Still not proven**, and worth treating with the old caution:
>
> - **Reliability of the two flaky screen-state checks above.** One run showed
>   them failing, the next run was clean; neither is proven absent, and this
>   document does not claim the suite is reliably green run over run.
> - **The consumer apps on v11, let alone v12.** Neither SpokenFeeds nor Snap2Ink
>   has been rebuilt against either. Everything above was driven by test
>   harnesses on a Mac, not by an iPhone. Under v12's clean break, both are
>   *refused* until they declare a shape and ship together with the firmware.
> - **`RENDER_STATUS(Superseded)`.** Deliberately untested — the window is small
>   enough that a test for it would be an intermittent race rather than a check
>   (see "Superseded pushes").
> - **Power and battery behaviour** of the connection-profile ladder over a real
>   discharge.
>
> `docs/companion-mode-implementation-notes.md` § "v6 bring-up log" remains the
> ranked risk list and the measured memory budgets.
>
> ### The periodic disconnect: root-caused 2026-08-04
>
> This block used to list "long-run link stability" as unexplained. It is not,
> and the answer is worth carrying because it is a trap any peripheral can fall
> into.
>
> `onConnect()` sent an `LL_LENGTH_REQ` (`NimBLEServer::setDataLen`) that on the
> affected connections went unanswered, and the Core Spec's **LL procedure
> response timeout of 40 s** then dropped the link with **HCI `0x22`**. Every
> such disconnect ever captured landed at **39992-40001 ms** into the
> connection — a spec constant, not radio conditions. (A 2026-08-06 sniffer
> capture shows iOS *completing* `LL_LENGTH_REQ → RSP` on a calm connection, so
> "iOS never answers" is too strong — the operative failure is the collision:
> the request raced the conn-param and PHY procedures fired alongside it and
> the central's own setup exchange, and a collided procedure expires at TPRT.) It also starved the two procedures fired alongside it: with it gone
> the PHY reaches 2M and the opening conn-param request is granted verbatim,
> neither of which used to happen.
>
> The rule to take away: **only one LLCP procedure may be pending on a connection
> at a time** (Core Spec Vol 6 Part B §5.3), so `onConnect()` — where the central
> is still running its own setup exchange — is the wrong place to start one. On
> 2026-08-07 the macOS central reproduced the collision exactly: a DLE-carrying
> build died at 39996 ms with HCI `0x22` (TPRT), request present, against
> `scripts/companion_e2e_test.py`; the same harness with the request absent
> then survived a 3-minute soak (`--soak 3`, session held foreground, two
> content-push rounds). The fix is host-A/B-provable after all, and was proven
> that way -- the earlier claim that a macOS harness "cannot show this" (macOS
> was assumed to self-negotiate DLE harmlessly) was wrong.
>
> **Same day, second pass: DLE is back, serialized.** `onConnect()` now starts
> only the conn-param procedure. `onConnParamsUpdate()` starts PHY, but only
> once `CompanionConnPolicy::inFlight()` reports the conn-param request itself
> matched and settled (not just *any* conn-param event — the central's own
> opening announcement fires this callback too, without matching, and
> correctly does not trip it). `onPhyUpdate()` then starts DLE
> (`setDataLen(251)`), gated on PHY's own callback having fired. Each step
> fires exactly once per connection. This is single-shot with no automatic
> retry on refusal: neither the Core Spec nor Apple's/Nordic's public guidance
> mandates a backoff-and-retry for a rejected LL Control Procedure, and
> NimBLE-Arduino's GAP event dispatcher (`NimBLEServer.cpp`'s
> `gapEventHandler`) never forwards `BLE_GAP_EVENT_DATA_LEN_CHG` in the first
> place, so there is no signal to retry against even if that were the policy —
> DLE's own completion is unconfirmable through this dependency as vendored.
> Proven on hardware: a 3-minute **and** a 5-minute soak (`--soak 3`,
> `--soak 5`) both survived cleanly with this chain in place — past the 40 s
> TPRT window 4-9× over with no `0x22`.
>
> **First A/B attempt was measured on the wrong path, and said so misleadingly.**
> `scripts/companion_e2e_test.py`'s image scenario deliberately pushes the
> image over Write *With* Response (its own docstring explains why: bleak has
> no cross-platform equivalent of CoreBluetooth's `canSendWriteWithoutResponse`
> flow control), which is the pre-v9 transport this codebase moved *away* from
> for exactly this reason (`3d2aec31`: 24.65 s → 1.83 s switching images to
> Write Without Response). Measuring a DLE A/B on that harness produced
> 19.9–20.1 s in both conditions — a real number, but for a transport no real
> client uses for images, dominated by the ~120 ms write-with-response round
> trip per chunk regardless of fragmentation.
>
> **Corrected measurement, same day: `companion-bench`** (the macOS-native
> harness in `CompanionKit`, https://github.com/DarkStarDS9/CompanionKit,
> `Sources/companion-bench` — built with `swift build -c release --product
> companion-bench`) drives the exact CoreBluetooth/Write-Without-Response path
> a real consumer app uses. Same 528×792/104,544-byte image, three runs each:
> **without DLE**, BLE transfer 1.87 / 2.66 / 2.64 s; **with the serialized DLE
> chain above**, 1.83 / 2.70 / 2.78 s. These match the historical figure
> (`3d2aec31`: 1.83 s) and disagree sharply with the Python-harness numbers —
> confirming those were a harness artifact, not the real transport's
> behaviour. Run-to-run variance (1.83–2.78 s) is larger than any gap between
> conditions: **DLE makes no measurable difference on the real client path
> either**, at this image size and the current 15 ms Session profile.
>
> **DLE's actual grant was independently confirmed — and that confirmation
> overturned the premise of the whole A/B.** A Sniffle capture
> (`sniff_receiver.py -s /dev/cu.usbserial-21230 -m <READER_MAC> -o out.pcap`
> — the `-S <name-substring>` filter documented below did **not** work here,
> it never matched despite the name being present in the device's
> `SCAN_RSP`; use `-m` with the MAC read from an unfiltered `-a` capture if
> the device's MAC is unknown) during a `companion-bench` run shows
> `LL_LENGTH_REQ`/`LL_LENGTH_RSP` completing with 251/251 octets granted, not
> once but twice in the same connection (once per PHY). Decoded with
> `Sniffle/python_cli/pcap_decoder.py` from Sniffle's own bundled `sniffle`
> package — no scapy or tshark needed, contrary to this investigation's first
> assumption.
>
> **Then the same capture was taken against the *DLE-free* build — the one
> whose `onConnect()` requests nothing — and it shows the exact same 251/251
> grant.** macOS's own CoreBluetooth central negotiates Data Length Extension
> on its own initiative, independent of whether this firmware ever asks for
> it. Every companion-bench A/B in this investigation was therefore comparing
> DLE-on against DLE-on — there was never a real DLE-off condition tested
> against this central, which is the actual reason no throughput difference
> ever showed up. (This is consistent with the 2026-08-06 sniffer capture
> noted above of iOS completing `LL_LENGTH_REQ → RSP` on its own on a calm
> connection — that was never a counterexample to "iOS/macOS never answers,"
> it was a preview of this: the central does this by itself.)
>
> This reframes the original 40 s TPRT bug too: it was very likely never
> "DLE is unsafe to request" in the abstract, but specifically **our own
> redundant request colliding with the central's own already-in-flight one**
> — both captures (DLE-on and DLE-free builds) show a benign
> `LL_REJECT_EXT_IND` collision near connect (error `0x2A`, Different
> Transaction Collision, rejecting a `LL_CONNECTION_PARAM_REQ` in one capture
> and a `LL_PHY_REQ` in the other) that self-resolves immediately with no
> disconnect — proving collisions near connect are routine on this link and
> normally harmless. The fatal case was specifically `LL_LENGTH_REQ` going
> *unanswered* rather than cleanly rejected; whether that is a controller-
> specific mishandling of that one collision shape, or something else, is not
> established by anything captured so far.
> (Both captures' own `Dir: C->P`/`P->C` labels look unreliable — every
> `LL_CONTROL` packet, including responses, was tagged the same direction,
> which cannot be correct — so which side originated which colliding
> procedure was not reliably determined this way; the octet counts, which do
> not depend on that field, are what proves the DLE grant itself.)
>
> **The periodic stalls that actually dominate transfer time are unrelated to
> DLE**, and present nearly identically in both builds: of ~600 back-to-back
> 189-byte chunk PDUs, most land ~1.14 ms apart (near the 2M-PHY radio-time
> floor), but roughly 1 in 5-6 is followed by a 9–30 ms gap instead — that
> gap, not per-packet fragmentation, is most of the wall-clock. The likely
> cause is this firmware's own `enqueueImageWork()` (`src/CompanionBle.cpp`):
> its 10-deep queue backpressures (briefly blocks) the BLE host task's
> `onWrite()` when the writer task's SD-card staging falls behind — a
> deliberate, documented design (see that function's and
> `kImageWriteQueueLen`'s comments) that trades a small, safe stall for never
> blocking the BLE radio task on SD I/O directly. `CONFIG_BT_NIMBLE_ACL_BUF_COUNT=12`
> (`sdkconfig.test`) is the matching controller-side buffer ceiling. CoreBluetooth
> is documented (Apple developer forum threads) to apply its own opaque
> internal batching/throttling to Write Without Response independent of
> anything the peripheral does, which may compound with the above; no source
> gives a queue depth or period for it, so it isn't confirmed as the dominant
> term here. Neither of these is instrumented yet — the numbers above are
> read directly from the Sniffle capture's packet timing, not from firmware
> or host-side counters.
>
> Net: the serialized chain is proven safe (soak) and its DLE step is proven
> to land (Sniffle) — but so does the DLE-free build's, from the central's
> own initiative, so **this firmware's own DLE request changes nothing
> observable**: not the grant (happens either way), not the throughput
> (identical either way, and the real bottleneck is the SD-write queue, not
> fragmentation). The strongest argument for keeping the serialized re-add
> at all is symmetry/documentation value (it makes this firmware's own
> intent explicit rather than silently free-riding on the central), against
> which it reintroduces one more procedure into the connect-time collision
> surface for a benefit that cannot currently be measured. Whether to keep it,
> simplify it away, or leave the decision open is not settled by this
> investigation. Anything added to `onConnect()` in future needs checking
> against the one-procedure-at-a-time rule regardless; `updatePhy()` is no
> longer there either now.

This is a **BLE peripheral/GATT-server role**, not something upstream
CrossPoint or this fork's `feat-bluetooth` branch already has — that branch's
BLE code is a HID *host* (X3 pairs to page-turner remotes as central), the
opposite role from what Companion Mode needs. See
`docs/companion-mode-implementation-notes.md` for the bring-up log;
`src/CompanionBle.h`/`src/CompanionBle.cpp` are the implementation, and
`docs/companion-multi-app-design.md` is the design this version implements.

A reference client implementation of everything below is `CompanionKit`
(SwiftPM package, iOS/macOS), maintained in its own repo at
https://github.com/DarkStarDS9/CompanionKit — extracted from this repo on
2026-08-07 with full history; both consumer apps depend on it via SPM. See
`clients/README.md`. Also: `scripts/push_companion_content.py`
(dev-machine Python, `bleak`).

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

**The list-state sync-back reaffirms that sentence rather than relaxing it.** It
has to move a payload — a shopping list's worth of
check-offs — that does not fit in one message, and the obvious shape for it
(one notification carrying every entry) is exactly what the rule forbids. It is
therefore a **paginated pull**: the device announces availability in one whole
message, the phone asks for a window in one whole message, and the device
answers each ask with one whole message. A sequence of independent messages is
not chunking. Each `LIST_STATE` is individually parseable, carries its own
`revision`/`offset`/`total`, and means something on its own; no message is a
fragment of another, and a client that drops one loses that window rather than
desynchronising a reassembly. That is what keeps the invariant true — there is
still no reassembly state on this characteristic, in either direction.

`HELLO` (up to 103 bytes) and `HELLO_OK` (up to 30 bytes) exceed BLE's minimum
MTU, so a handshake needs an ATT MTU of at least 106. The device requests 185
and both iOS and Android negotiate well above the floor in practice; a central
that cannot get past 23 cannot use v8 at all. Every message added since is sized
against that same 103-byte floor, `LIST_STATE` (101 bytes at its largest)
included, so no message on this characteristic has ever required an MTU a
`HELLO` did not already require.

### Phone → device (write)

```
0x01 HELLO      helloTag[2]  protocolVersion:1  appId[16]  installId[16]
                tokenLen:1  token[tokenLen]         (tokenLen 0 or 16)
                nameLen:1   name[nameLen]           (UTF-8, <= 24 bytes, may be empty)
                userNameLen:1  userName[userNameLen] (UTF-8, <= 24 bytes, may be empty)
0x02 BYE        sessionId
0x03 ACQUIRE    sessionId
0x04 RELEASE    sessionId
0x05 LIST_STATE_GET sessionId  offset:2
```

`protocolVersion` (v12) is the client's own protocol version — the current
value of `PROTOCOL_VERSION` (`scripts/companion_protocol.py`) /
`CompanionProtocol.version` (CompanionKit) / `kProtocolVersion`
(`src/CompanionBle.h`), the same one the capability characteristic already
reports device-side. Checked first, before anything else in the payload is
even assumed to be laid out where this build expects — a mismatch is denied
`PROTOCOL_MISMATCH` immediately, without parsing `appId`/`installId`/the rest.
This field's wire offset (right after `helloTag`) is fixed for the life of the
protocol for exactly that reason: whatever else a future version changes, this
one field never moves, so a version mismatch can always be caught this early.

A compliant client is expected to catch this itself first, by reading the
capability characteristic before ever writing `HELLO` (see "Capability
characteristic" above) — this field is the device-side backstop for that
contract, not a replacement for it. Before v12 there was no such field, and no
such backstop: a stale (pre-v12) client's declaration could only be refused
after the fact, once its missing shape byte made it fail to parse as a v12
declaration — the device had to *infer* "this looks like a v11 client" from a
buffer shaped like one. `REJECTED_NO_SHAPE` no longer carries that inference:
a stale client is now refused here, at `HELLO`, before it ever reaches a
declaration push. `REJECTED_NO_SHAPE` still exists and still fires for what it
was always the more honest answer for — a compliant client whose declaration
is otherwise sound but carries a shape byte with no known value (0x00, or
above `LIST`) — see `docs/companion-declared-shape-design.md` section 3.

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
0x88 RENDER_STATUS  sessionId  result:1  pushId:1              -- widened in v11
0x89 IMAGE_CHUNK_ACK sessionId seq:2                          -- new in v9
0x8A FIELD_SEQ_GAP  sessionId  field:1                        -- new in v10
0x8B LIST_STATE_AVAIL sessionId  revision:4  count:2          -- v12
0x8C LIST_STATE     sessionId  revision:4  offset:2  total:2
                    n:1  n x { itemId:2  checked:1 }          -- v12
```

```
HELLO_DENIED reason      0x00 USER_REJECTED     user pressed BACK on the prompt
                         0x01 TIMEOUT           no answer within ~30s
                         0x02 NO_SESSION_SLOTS  4 sessions already live on this link
                         0x03 MALFORMED         unparseable HELLO
                         0x04 STORAGE           SD unavailable / peer dir could not be created
                         0x05 BUSY              another pairing prompt is already on screen
                         0x06 PROTOCOL_MISMATCH protocolVersion doesn't match kProtocolVersion -- v12

BACKGROUND reason        0x00 PREEMPTED         another session acquired the screen
                         0x01 RELEASED          this session released it
                         0x02 LINK_LOST         (informational; not deliverable in practice)

ACQUIRE_DENIED reason    0x00 NO_UI_DECLARATION push field 0x05, then retry
                         0x01 UNKNOWN_SESSION   no such live session

ASSET_ACK result         0x00 STORED
                         0x01 REJECTED_SIZE     asset larger than the advertised limit
                         0x02 REJECTED_FORMAT   unparseable for that asset id
                         0x03 REJECTED_STORAGE  SD write failed
                         0x04 REJECTED_NO_SHAPE the UI declaration (0x05) carried no content
                                                shape byte, or one that is not 0x01/0x02/0x03.
                                                Distinct from REJECTED_FORMAT on purpose -- it
                                                is what every v11 client gets, and "missing its
                                                shape byte" is a far better thing to read in a
                                                log than "malformed". Nothing is stored, so the
                                                peer's next ACQUIRE is denied NO_UI_DECLARATION
                                                -- see "UI declaration field" (v12)

RENDER_STATUS result     0x00 DISPLAYED
                         0x01 DECODE_FAILED     wrong byte count for a raw 2bpp full-screen image,
                                                or a malformed list document (0x08: truncated,
                                                over-long counts, or a `checked` byte outside
                                                {0,1}) -- see "List document field"
                         0x02 REJECTED_SIZE     exceeded max image length, or a list document (0x08)
                                                over kMaxListDocLen bytes -- see "List document field"
                         0x03 STORAGE_FAILED    could not stage to SD (image), or could not write
                                                lists.bin (list document)
                         0x04 SEQUENCE_GAP      a CHUNK's sequence number skipped ahead of what
                                                was expected (image), or a title/body/tag batch
                                                was discarded because one of its fields hit that
                                                -- see "Image field" and "Atomic multi-field pushes"
                         0x05 SUPERSEDED        a later push took the screen before this one
                                                reached the panel, so it never rendered. Not an
                                                error -- see "Superseded pushes" below
                         0x06 REJECTED_SHAPE    the pushed field is not one this peer's declared
                                                content shape permits (v12): an image from a TEXT
                                                peer, a title/body/content-id field from an IMAGE
                                                peer, a list document (0x08) from a TEXT or IMAGE
                                                peer, or any content field other than 0x08 from a
                                                LIST peer. Tag state (0x07) is an overlay and is
                                                permitted under both TEXT and IMAGE, but NOT LIST
                                                -- see "List document field".
                                                Latched when the offending START arrives -- before
                                                any buffer is allocated -- and answered at END, so
                                                a whole batch of illegal fields is still answered
                                                exactly once. Nothing is rendered and the screen
                                                is left as it was. See "UI declaration field"

RENDER_STATUS pushId     Whatever the pushing client sent as END's third byte
                         for the push this answers (see "Content characteristic"
                         below) -- the device never interprets this value, only
                         echoes it back. For a multi-field atomic batch, this is
                         specifically the pushId from the *final*-flagged
                         field's END; every other field's pushId is not looked
                         at, since the batch is answered exactly once. pushId
                         0 means the pushing client did not want an answer at
                         all, and the device never sends RENDER_STATUS for a
                         push whose final END carried 0.
```

`IMAGE_CHUNK_ACK` is a progress marker only, sent roughly every 32 CHUNKs
during an image push (see "Image field" below) — it is not required for
correctness and a client that ignores it loses nothing but early failure
detection. `seq` is the highest contiguous CHUNK sequence number the device
has processed.

`FIELD_SEQ_GAP` is new in v10: sent when a title (`0x01`) or body (`0x02`)
push's `CHUNK` sequence number skips ahead of what the device expected — the
signature of a packet dropped or reordered under Write Without Response (see
"v10 changes from v9" below). Unlike the image field there is no mid-transfer
ack (a text push is a handful of chunks, not hundreds) and no partial-resume
protocol — the device drops the whole field rather than render a spliced
page, and the client's only path forward is re-pushing it from a fresh
`START`. `field` is the field id that was dropped. If the dropped field was
part of an atomic batch, the **whole batch** is discarded — see "Atomic
multi-field pushes" below — so the correct recovery is to re-push the entire
batch, not just the field named here.

`ASSET_ACK`, `RENDER_STATUS` and `FIELD_SEQ_GAP` are the things a client
genuinely cannot work out for itself: whether the device stored the asset,
whether a push actually reached the panel, and whether a title/body push
survived the trip intact. Everything else about rendering is deterministic
from what was pushed.

`RENDER_STATUS` — named `IMAGE_STATUS` through v10, when it only ever answered
an image push; widened in v11 to also answer a title/body/content-id/tag
content batch, on the same opcode (`0x88`) with one appended byte rather than a
second opcode. Motivation: a v10 client had no way to know when pushed text
was actually visible on the panel — the wire transfer for a text push
completes in ~0.24s, but the panel's own multi-pass grayscale/refresh settle
takes a further ~2.2s measured on hardware, and only an image push got an
answer once that settle finished. `RENDER_STATUS` is **strictly a response to
a push**, never a broadcast about what is on screen. Exactly one arrives per
push that asked for one (a pushed field `0x04`, or a committed
title/body/content-id/tag batch), and none at all for a redraw the client did
not cause — connecting while older content is still displayed, a foreground
handover, a tag-only redraw, or the user paging the device's local gallery all
repaint the panel silently. A client correlates the answer to the push that
earned it via `pushId` (see "Content characteristic" below) rather than by
guessing from field identity.

That appended byte was, for exactly one day of this same v11 release, a
`field` id (`0x04` for an image push, `0x02` — standing in for "the batch",
since there is no single field id for one — for a text batch). It was replaced
before any consumer app had adopted it, because it does not generalize: every
future push type would need its own field id, and a client juggling two
outstanding pushes still has to trust the device never answers them out of
order. `pushId`, chosen by the *pushing client* and simply echoed back,
generalizes to any future push type for free and distinguishes two pushes a
`field` byte could not tell apart at all: two successive article batches from
the same TEXT peer, where an answer must be attributable to *which article*, not
to what kind of push it was. (Before v12 this paragraph reached for "an image
and a text batch" as the example. A single peer can no longer push both — it
declares one content shape — so the case `pushId` actually earns its keep in is
two pushes of the *same* shape, plus the general rule that the device owes
exactly one answer per push and a client has to know which push it answered.)

**v10 and earlier clients must not be fed a v11 `RENDER_STATUS`.** A v10
parser reads this notification as a fixed 3 bytes (`{opcode, sessionId,
result}`); a text push now also emitting it, plus the appended `pushId` byte,
both change what a v10 client would observe on the wire for an opcode it
already knew — which is exactly why this is a protocol version bump (10 → 11),
not a silent behavior change. See "Version history" below.

### List-state sync-back — availability notify, phone-driven pull

The device's ToDo List check-off diff (see "List document field" below) travels
back to the phone entirely on this characteristic. There is **no new content
field for it** — `0x09` is still free — because this is a conversation the
device initiates, the phone answers piecewise, and which carries no
screen-owning content, none of which `START`/`CHUNK`/`END` field framing
models.

```
device -> phone  0x8B LIST_STATE_AVAIL  sessionId:1  revision:4  count:2
                                                                       (8 bytes)

phone  -> device 0x05 LIST_STATE_GET    sessionId:1  offset:2
                                                                       (4 bytes)

device -> phone  0x8C LIST_STATE        sessionId:1  revision:4  offset:2
                                        total:2  n:1
                                        n x { itemId:2  checked:1 }
                                                                (11 + 3n bytes)
```

All multi-byte fields little-endian, as everywhere else on this wire.

**Entries are deviations from the document at `revision`, not absolute
checkbox states.** An entry says "the user set `itemId` to `checked`, and the
document said otherwise"; an item the user never touched, or toggled back to
what the document said, has no entry at all. `revision` is the document
revision the deviations were taken against — the device never interprets it,
and never decides whether its own diff is still applicable. Merging is the
phone's job (see "dumb firmware, smart phone" in `CLAUDE.md`), and the phone
finishes by pushing a new `kFieldListDoc` at a new revision, which clears the
diff.

**`n` is at most `kListStateEntriesPerNotify` = 30**, making `LIST_STATE` at
most 101 bytes — inside the 103-byte session floor every central already has to
clear for `HELLO`. So a full pull needs no MTU renegotiation, and the device
never queries the negotiated MTU. **The 30 is fixed, deliberately not derived
from the MTU**, for the reason this doc already gives for `kMaxContentIdLen`
("treat 32 bytes as the contract"): a constant is host-testable and identical
for every client, whereas a per-connection page size would make the pagination
boundary — the one thing a pull's correctness turns on — a property of
whichever central happened to connect.

**The pull is stateless.** Each `LIST_STATE_GET` is an independent seek into
the device's stored diff; no cursor is held between requests. A phone may
repeat a window, request windows out of order, abandon a pull halfway, or start
again from `offset 0` on the next connection, and none of that is a state the
device has to unwind.

**`n = 0` is the terminator, not an error.** A `LIST_STATE_GET` whose `offset`
is at or past `total`, or that names a session the device does not know, or
that arrives for a peer with no stored diff, is answered `LIST_STATE` with
`n = 0` (`revision`/`total` as known, both `0` when they are not). The device
logs nothing and there is no error result: "read until `n` is 0, or until
`offset + n` reaches `total`" is the whole client-side loop.

**When `LIST_STATE_AVAIL` is sent — and the gate.** The device sends it **if
and only if** the peer has a stored diff with at least one entry. There is no
capability bit for "this peer does ToDo lists": a non-`LIST` peer structurally
cannot have a diff, since without a stored list document there is no on-device
screen from which anything could be toggled, so "has a non-empty diff" is the
entire gate. A `LIST` peer that has never had anything checked off simply hears
nothing, which is a better answer than an empty message the phone has to
interpret.

It is sent at three moments:

- **immediately after `HELLO_OK`**, before the phone can push anything;
- **alongside `FOREGROUND`**, when the peer takes the screen;
- **live**, when the user checks something off while that peer is connected.

**The `HELLO_OK` ordering is load-bearing.** Storing a new list document clears
the peer's diff unconditionally — the device does not get a say in whether its
own edits still apply — and a document push requires `ACQUIRE` and the
foreground, neither of which can happen before `HELLO_OK`. Announcing at
`HELLO_OK` therefore means the phone always learns a diff is pending *before*
it is capable of destroying one. A client that pushes a document before pulling
is not corrupting anything the device can detect; it is discarding edits it was
told about.

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

### Framing (v6, `CHUNK` amended in v9 for the image field and v10 for title/body; `END` amended in v11)

```
START:  byte 0      opcode = 0x01
        byte 1      field | final-flag   (low 7 bits = field id; 0x80 = "final field of this push")
        byte 2      sessionId
        bytes 3..6  total payload length, uint32 little-endian

CHUNK:  byte 0      opcode = 0x02
        byte 1      sessionId
        bytes 2..N  payload bytes                    (content-id, UI declaration, icon, tag state)

CHUNK:  byte 0      opcode = 0x02
        byte 1      sessionId
        bytes 2..3  sequence number, uint16 little-endian, starting at 0
        bytes 4..N  payload bytes                    (image, title, body — v9+/v10+, see
                                                       "Image field" and "Title/body fields")

END:    byte 0      opcode = 0x03
        byte 1      sessionId
        byte 2      pushId
```

A push of one field is: one `START` declaring the field and its total byte
length, one or more `CHUNK`s carrying the bytes in order (each sized to the
negotiated MTU minus the CHUNK framing overhead — 2 bytes, or 4 for image,
title and body), then one `END`. Fields are independent pushes over the same
characteristic — send one field's full START/CHUNK…/END before starting the
next. The device does not assume an order beyond "each field is internally
ordered".

**`END`'s `pushId` (v11, required) is chosen by the client and simply echoed
back on `RENDER_STATUS` — the device never interprets it.** A 2-byte `END`
(the only shape that ever existed before v11) is now malformed and rejected
the same way any other too-short frame on this characteristic is, logged and
dropped rather than tolerated as an optional trailing byte: these are all
first-party clients (this repo's own Swift package and Python scripts), so
there is no outside caller to stay backward-compatible with, and carrying an
"`END` might be 2 or 3 bytes" branch forever would be a permanent tax for a
distinction that stopped existing the same day it was introduced. Two rules
govern the value:

- **`pushId` 0 means "I am not awaiting a `RENDER_STATUS` for this push."** The
  device never sends one for a push whose relevant `END` (see below) carried
  0 — arming an answer nobody asked for would just be wasted notification
  traffic on every push that doesn't care, which in practice is most content
  pushes and every UI-declaration/icon asset push.
- **For an atomic multi-field batch (see "Atomic multi-field pushes" below),
  every field's `END` carries a `pushId`, but only the *final*-flagged
  field's is retained** — that is the id `RENDER_STATUS` answers the whole
  batch with. A client sends the same `pushId` on every field of one batch in
  practice (there is no reason to vary it), but the device does not require
  that; it simply never looks at a non-final field's value.

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
- Fields `0x01` `0x02` `0x03` `0x04` `0x07` `0x08` (title, body, content-id,
  image, tag state, list document) are dropped unless the sending session
  currently holds the screen.

Frames from an unknown session are always dropped. Pushing a UI declaration
while another app holds the screen stores it silently; it takes effect for you
when you next acquire.

A new `START` discards any partial reassembly in progress. Reassembly is also
discarded on disconnect and on a foreground handover.

### Field ids

| Id | Field | Cap | Reassembled into | Shape (v12) |
|---|---|---|---|---|
| `0x01` | title | max text length (capability) | RAM | TEXT |
| `0x02` | body | max text length (capability) | RAM | TEXT |
| `0x03` | content-id | 32 bytes | RAM | TEXT |
| `0x04` | image | max image length (capability) | **streamed to SD**, never buffered in RAM | IMAGE |
| `0x05` | UI declaration (shape + buttons + tags) | 512 bytes | SD (`ui.bin`) | *asset — never checked* |
| `0x06` | icon | icon width x height / 8 bytes | SD (`icon.bin`) | *asset — never checked* |
| `0x07` | tag state | 13 bytes | RAM (foreground only) | TEXT + IMAGE |
| `0x08` | list document | 16 KB (`kMaxListDocLen`) | heap, transient — reassembled for the duration of one push, validated, written straight through to `lists.bin` on SD as the exact bytes received, then freed; never resident | LIST |

Next free: `0x09` — the list-state sync-back did not consume it, being three
Session-characteristic opcodes rather than a content field (see "List-state
sync-back").

The last column is v12's permitted-field table: a peer may push a content field
only if its declared content shape matches, and anything else is answered
`RENDER_STATUS(REJECTED_SHAPE)` — see "UI declaration field". The two asset
fields are exempt by construction: pushing `0x05` is how a peer changes its
shape in the first place. Tag state (`0x07`) is exempt in a different way: it is
an overlay drawn *over* whatever content is on screen rather than content of its
own, so both content shapes permit it. `LIST` peers permit exactly one content
field, `0x08` — **not** `0x07`: unlike `TEXT`/`IMAGE` there is no list screen to
overlay a tag chip onto yet, so the overlay exemption does not extend to `LIST`
(see "List document field" below).

A list document is **content, not an asset** — it is answered on `RENDER_STATUS`
like title/body/image, never `ASSET_ACK` (which is reserved for the two fields
above, `0x05`/`0x06`, that are per-peer state rather than screen content).

Content past a field's cap is truncated (title/body/content-id) or rejected
outright with `ASSET_ACK`/`RENDER_STATUS` (image, UI declaration, icon, list
document) — a truncated asset is worse than no asset, and for a list document
specifically a truncation is worse than for text: landing on a structural
boundary would parse as a shorter but well-formed document and silently drop
items with no error at all, so an over-cap push is refused outright
(`RENDER_STATUS(REJECTED_SIZE)`) rather than truncated to the cap the way
title/body are. `kMaxListDocLen` is the only cap on this field — see "List
document field" below for why an earlier revision also capped total item
count, and why that second cap no longer exists.

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

**A batch that loses a field is discarded whole.** If a title or body field in
the batch is dropped for a CHUNK sequence gap (`FIELD_SEQ_GAP`, see below), the
device does not commit the surviving fields when the final flag arrives: it
throws the batch away and leaves the previous content on screen. Committing the
survivors would render this push's body under the *previous* push's title — a
fresh story under a stale headline — which is worse than showing a page that is
merely out of date. The same applies to the 3 s timeout path: a batch that lost
a field is discarded there too, not applied. `FIELD_SEQ_GAP` is unchanged and
still names only the field that was actually lost; the client's recovery is to
re-push the whole batch.

"Whole" includes tag state (`0x07`) pushed inside the batch: it is dropped with
everything else, because the alternative is the stale article on screen wearing
the *new* article's tags — a mark the user can see attached to content they
cannot. A tag state pushed **on its own**, outside any title/body batch, is
unaffected and still applies immediately; only tag state that arrived as part
of the poisoned batch is discarded.

**A batch containing a field the peer's shape does not permit is refused whole,
and answered once (v12).** The refusal is latched when the offending `START`
arrives — before any buffer is allocated — and answered at the batch's
final-flagged `END` with `RENDER_STATUS(REJECTED_SHAPE)`, carrying that `END`'s
`pushId`. It is deliberately *not* answered per illegal field: a three-field
batch from a peer of the wrong shape produces exactly one notification, the
same "one answer per push" contract every other result obeys, and a client
counting answers against pushes must not have to special-case this one. As
with a sequence gap, nothing is committed and the previous content stays on
screen. And as everywhere else, `pushId 0` still means "no answer wanted" —
a refused batch pushed with `pushId 0` is silently dropped, not reported.

On the client side this is a small, fully synchronous send loop — there is no
per-chunk ack. If reliable delivery matters, use "Write" (not "Write Without
Response") for the CHUNK packets so BLE's own link-layer ack applies.

**Except the image, title and body fields, where the recommendation is the
opposite: push their CHUNKs with Write Without Response.** Measured on real
hardware (ESP32-C3, 15ms connection interval, 2M PHY): a write-with-response
round trip costs ~120ms *regardless of connection interval* — the peripheral's
own handling is 0-1ms, so the cost is the ATT round trip itself, not anything
the firmware does. For the image field (v9) that floored a ~200-chunk transfer
at several times the link's real throughput; for title/body (v10) the same
cost, paid per chunk of a full page of text, is what made rapid-fire content
updates (a new article every few seconds, each a full page) visibly fall
behind — the chunk count is far smaller than an image's, but so is the budget,
since the whole push has to land before the next one starts. Write Without
Response has no such round trip, but drops CoreBluetooth/BlueZ's own delivery
guarantee, which is why these fields' CHUNKs carry the sequence number
described above: a dropped or reordered chunk is now something the device
*detects* — `RENDER_STATUS(SEQUENCE_GAP)` for the image field,
`FIELD_SEQ_GAP` for title/body — instead of something that silently corrupts
the reassembled payload. Keep `START` and `END` on Write, so the phone still
gets a reliable begin/end ack. Every other field is unaffected — small enough
that the per-chunk round trip this exists to avoid barely matters, and (having
no sequence number) still depends on Write's link-layer ack for correctness.

A client pushing over Write Without Response should throttle to what the OS
buffers for un-acked WWR writes (iOS: `CBPeripheral.canSendWriteWithoutResponse`
/ `peripheralIsReady(toSendWriteWithoutResponse:)`) rather than writing in a
tight loop. `IMAGE_CHUNK_ACK` (above) is a diagnostic on top of that for the
image field, not a substitute for it, since it arrives only every ~32 chunks
and says nothing about how many writes the OS will currently accept; title and
body pushes are short enough (a handful of chunks) that no equivalent
mid-transfer ack exists for them — `FIELD_SEQ_GAP` (or its absence) at `END`
is the only signal.

### Title/body fields (`0x01`/`0x02`) — sequence-checked CHUNKs (v10+)

Plain text, up to the max text length advertised in the capability
characteristic (bytes 3..4). No wire encoding beyond that — send UTF-8 bytes,
the device wraps and truncates for display.

**As of v10, both fields' `CHUNK`s carry the 2-byte sequence number described
in "Framing" above**, the same treatment the image field got in v9, and for
the same reason: pushed over Write Without Response, they need a way to
detect a dropped or reordered packet instead of silently splicing the wrong
bytes together. The device tracks the next expected value (reset to 0 at
`START`, shared with whichever field is currently being reassembled — only
one field is ever in flight at a time) and, the instant a `CHUNK` arrives out
of sequence, marks the field corrupt and notifies `FIELD_SEQ_GAP` at `END`
instead of handing a partial or spliced buffer to the renderer. As with the
image field, there is no partial-resume protocol: recovering from
`FIELD_SEQ_GAP` means re-pushing the field from a fresh `START` — and, if the
field was part of an atomic batch, re-pushing the whole batch, since the device
discards a batch that lost a field (see "Atomic multi-field pushes").

Unlike the image field, title/body pushes are small enough (a handful of
chunks for a full page, not hundreds) that no mid-transfer progress ack
exists for them — `FIELD_SEQ_GAP`, or its absence, at `END` is the only
signal. A client that needs to know a push landed clean before moving on
should wait for `END`'s round trip (still Write, hence acked) and watch for
`FIELD_SEQ_GAP` in that window.

**As of v11, a title/body/content-id/tag batch also gets a completion signal:
`RENDER_STATUS(Displayed, pushId)`, once the batch actually reaches the
panel** — not merely once the wire transfer finishes. This is the answer to
"is my last push actually showing yet", not "did my last push land intact"
(that's still `FIELD_SEQ_GAP`/`END`'s round trip, above). The gap between the
two matters: the wire transfer for a full-page text push completes in ~0.24s,
but the panel's own settle (page layout + e-ink refresh cycle) measured a
further ~2.2s on hardware. A client that only watched for the wire transfer to
finish — the only option before v11 — could not tell those apart, and had to
guess with a fixed delay. If the batch is discarded whole (see "A batch that
loses a field is discarded whole" above), `RENDER_STATUS(SequenceGap, pushId)`
is sent immediately instead, since no render is ever coming for it — see
`FIELD_SEQ_GAP` above for the field-level version of the same signal.
(`pushId` here is the one from the batch's final-flagged field's `END`, per
"Framing" above — or the device sends nothing at all if that was 0.)

#### Superseded pushes

**The device owes exactly one `RENDER_STATUS` per push that asked for one,
including when that push never renders.** A content push selects the text
render branch and an image push selects the image one, so if a second push
arrives before the first has reached the panel, the second one's content is
what gets drawn and the first one's render never happens. The superseded push
is answered with `RENDER_STATUS(Superseded, pushId)` — using *its own*
`pushId`, not the push that overtook it — at the moment it is overtaken, so a
client awaiting it fails fast instead of waiting out its own timeout for an
answer that was never coming. (If the superseded push's `pushId` was 0, this
is simply not sent, same as any other answer to a push that asked for none.)

`Superseded` is **not an error**. Nothing failed and nothing was corrupted — the
content was simply overtaken by something newer, which is very often exactly what
the app intended. Do not retry on it: the app has already moved on, and re-pushing
would race whatever superseded it and could put stale content back on the panel.

**Expect this to be rare.** A client that does not await the render is done with a
push as soon as its wire transfer completes (~0.24s) while the render it triggered
still has ~1.7s to run, so an image push can start mid-render — but the device
serialises the two behind its render lock, so the earlier push normally still
reaches the panel and reports `Displayed`. Measured on hardware: a text push
followed 0.3s later by an image answered `Displayed`, not `Superseded`. The
remaining window is the sliver where an image finishes staging and takes the
screen before the queued text render has begun. Handle `Superseded` because it is
cheap to, not because it is likely.

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
`RENDER_STATUS` reports the outcome.

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
  `RENDER_STATUS(SEQUENCE_GAP)` the instant a CHUNK arrives out of order — a
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
payload of the wrong byte count is rejected outright (`RENDER_STATUS`
`DECODE_FAILED`) rather than resampled or cropped. Cropping, scaling and
rotation are entirely the phone's job; the device never does any of the
three for an image push.

For this device's measured 528 x 792 panel: `bytesPerRow = ceil(528/4) = 132`,
so every push is exactly `132 * 792 = 104544` bytes — fixed, with no
compression-dependent variance and no worst-case blowup risk.

**Interaction with text (rewritten for v12).** An image push replaces the screen
entirely — title, body and paging are not drawn while an image is displayed —
and there is still no compositing of the two. What changed in v12 is *which peer
may push what*: a peer that has declared `IMAGE` may push field `0x04` (plus the
overlay field `0x07`) and nothing else, and a peer that has declared `TEXT` may
not push an image at all.
Pushing a body after an image no longer "returns the screen to text"; from an
`IMAGE` peer it is refused outright with `RENDER_STATUS` `REJECTED_SHAPE`, and
from a `TEXT` peer the image was never accepted in the first place.

So there *is* now something a client enters and leaves, and it is neither
implicit nor per-push: it is the content shape in the peer's UI declaration. It
changes only when the app re-declares itself, which clears the screen. Through
v11 the model was reactive — the last completed push of either kind owned the
screen — and this paragraph said so. That model is gone: a passive text page and
a passive image could replace each other harmlessly, but a screen holding local
user state (a list's cursor, its scroll offset, its not-yet-synced check-offs)
cannot, and the firmware needs to know a peer's shape while disconnected, which
no rule about "the last push" can supply. See "UI declaration field" above.

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

**v12: tag state is an overlay, so field `0x07` is legal for an `IMAGE` peer
too.** The shape table says what *content* a peer pushes, and a tag is not
content — it is a chip drawn over whatever content is on screen. So `0x07` is
exempt from the one-shape-one-field-kind rule and is permitted under both
`TEXT` and `IMAGE`: an `IMAGE` peer may push image + tag state as a single
atomic batch, exactly as a `TEXT` peer pushes title + body + tag state. (`LIST`
permits exactly one content field, the list document `0x08` (see "List document
field") — `0x07` is still excluded, because there is no list screen to overlay
a tag chip onto yet.) The **Status characteristic** write (see "Status
characteristic" below) is not a content-field push and is not shape-checked
either, so both routes stay open to a peer of any shape: use the field when the
content is changing too, the Status write when only the chip is.

**On timing, if you want a "finished developing" mark.** A tag pushed *with* the
image is drawn when the image is drawn, which is the *start* of the grayscale
settle, not the end. If you want a mark that means "this print has finished
resolving", set it with a standalone Status write after `RENDER_STATUS(DISPLAYED)`
arrives — that notification is sent after the settle completes, and the redraw it
triggers is the cheap chips-only one described above. The device will not infer
this for you: when a tag means "done" is your semantics, and a firmware that
filled a mark because it decided that is what the app meant would be
interpreting.

### UI declaration field (`0x05`)

Everything the app declares about its own on-device UI: **what kind of content
it pushes**, what its buttons do and what they are called, and what tags exist
and what they are called. One asset, one digest.

```
bytes 0..3   asset digest (opaque, stored verbatim — see "Asset digests")

byte 4       content shape              <- MANDATORY, new in v12
byte 5       button entry count N
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

#### Content shape (byte 4) — mandatory

```
0x01  TEXT    may push title (0x01), body (0x02), content-id (0x03), tag state (0x07)
0x02  IMAGE   may push image (0x04), tag state (0x07)
0x03  LIST    may push a list document (0x08) and nothing else -- not even tag
              state (0x07); see "List document field"
```

Tag state appears under both shapes on purpose: it is an overlay, not content —
see "Tags are drawn over an image" above.

A peer declares **one** shape and may push only the content fields belonging to
it, for as long as that declaration stands. A field outside it is refused with
`RENDER_STATUS` `REJECTED_SHAPE` (see below); nothing is rendered and nothing is
stored.

`0x00` and anything above `0x03` are **invalid, not reserved**. An unknown shape
is refused rather than tolerated, because tolerating it would mean falling back
to "render whatever arrives", which is exactly the behaviour v12 removes.

**Mandatory means mandatory: there is no unset value and no default.** A
declaration without this byte does not parse, so it is not stored, and is
answered `ASSET_ACK` `REJECTED_NO_SHAPE` (`0x04`). The peer then has no stored
declaration at all, which the existing structural gate already covers: its
`ACQUIRE` is denied `NO_UI_DECLARATION`. No new `ACQUIRE_DENIED` reason exists,
because none is needed — *a peer that has not declared itself cannot reach the
screen* simply extends to cover shape.

**Why it is here and not on `ACQUIRE`.** The device has to know a peer's shape
while **disconnected** — the idle icon grid opens a peer's stored content with
no session in existence — and a per-session byte cannot answer that. This asset
is already per-peer, already persisted, already versioned by a digest, and
already read while disconnected. Shape and buttons also belong together: a list
peer's Up/Down/Confirm meanings are a consequence of it being a list peer.

**Why it is at the front and not appended.** The declaration's trailing fields
signal absence by the buffer running out, so a *mandatory* field cannot sit
behind optional ones — given a single trailing byte, a decoder cannot tell a
shape from a tag render style. Byte 4 is free precisely because v12 is a clean
break, and it leaves the trailing-optional convention intact for the two fields
that legitimately use it.

**Changing shape is possible, deliberately and only deliberately.** Re-pushing
this declaration with a different shape while foreground is the sanctioned way,
and it is not free: a re-pushed declaration triggers a foreground change, which
reloads the button map and **clears the screen**. That is the intended cost
model. An app that genuinely needs both shapes (a reader that wants to show one
hero image) can do it; it just cannot do it by accident, and the device always
knows the current answer without inspecting content.

**The declaration and icon fields are never shape-checked.** Pushing field
`0x05` is precisely how a peer changes its shape, and both it and the icon
(`0x06`) are accepted from a non-foreground session. Shape-checking either would
deadlock a peer whose stored shape is wrong out of ever fixing it.

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

**`IMAGE_GALLERY` and the v12 content shape are not the same thing, and both
stay.** The shape byte says what a peer may *push*; this bit says whether it
wants a *tile in the on-device gallery picker*. They will normally agree — an
`IMAGE` peer sets the bit — but the bit is not derived from the shape and the
firmware does not infer one from the other. A `TEXT` peer that sets it is not an
error; it simply gets a picker tile it will never fill, which is a strictly
better failure than the device quietly deciding what an app meant.

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

### List document field (`0x08`) — ToDo List

**This field is push-only.** It carries a whole
document from the phone to the device, and nothing travels back on it. The
device's check-off edits go back over the Session characteristic instead — see
"List-state sync-back" above — which is why the round trip needed three
opcodes and no second content field. The document in `lists.bin` is never
mutated by the device: edits are recorded as deviations from it, so what the
phone sent stays distinguishable from what the user did.

A `LIST` peer's UI declaration permits exactly this one content field (see
"Field ids" above) — pushed as a whole-document replace, like the UI
declaration and icon assets, not as incremental add/remove-item operations.

**Wire layout**, copied from `src/CompanionTodoDocument.h`, the single source
of truth for this format:

```
u32  revision                       (little-endian)
u8   list count L
L x {
  u16 listId                        (little-endian)
  u8  titleLen, title[titleLen]     (UTF-8, not NUL-terminated)
  u8  group count G
  G x {
    u16 groupId                     (little-endian)
    u8  labelLen, label[labelLen]   (empty label == the list's "ungrouped" bucket)
    u8  item count I
    I x {
      u16 itemId                    (little-endian)
      u8  checked                   (0 or 1 ONLY; any other value is malformed)
      u8  textLen, text[textLen]
    }
  }
}
```

All multi-byte fields are little-endian, matching every other multi-byte field
already on this wire (`START`'s payload length, the image `CHUNK` sequence
number, the button-hold duration field) — this format follows that existing
convention rather than introducing a new one. A document with trailing bytes
left over after the last item, or one whose count claims run past the end of
the buffer, is malformed.

- **`listId`/`groupId`/`itemId` are device-opaque `u16`, not `u8`.** The device
  only ever compares and echoes them, never generates or interprets one — same
  rule as `appId`/`installId`/content-id elsewhere on this wire. They are
  `u16` specifically because `kMaxListDocLen` (16 KB, below) comfortably admits
  more than 255 items in one legal document; a `u8` id would silently overflow
  inside an otherwise well-formed push.
- **An empty group label means the list's "ungrouped" bucket** — there is no
  separate bucket type on the wire or in storage, one less case for the
  renderer.
- **`checked` must be exactly `0` or `1`.** Any other byte value makes the
  whole document malformed (`RENDER_STATUS(DECODE_FAILED)`) rather than being
  clamped or ignored — the device never writes this byte back into the stored
  document, so accepting a bad one would mean rendering state it cannot
  explain.
- **`checked` here is the phone's value, not necessarily what is on screen.**
  What the device renders is this byte overridden by any local deviation
  recorded in `list_state.bin` (see "List-state sync-back"). Storing a new
  document clears those deviations, so immediately after a push the two agree
  by construction.

**One cap, `kMaxListDocLen` = 16 KB, bounds the transfer's total bytes.** A
push over this is **refused outright at `START`** — `RENDER_STATUS(REJECTED_SIZE)`
— **not silently truncated to the cap** the way title/body are. Truncating an
arbitrary byte off a structured document can land exactly on a
list/group/item boundary and parse as a shorter but perfectly well-formed
document, silently dropping items with no error the app or the user would
ever see; refusing the whole push instead makes that failure loud.

There used to be a second cap here, `kMaxListItems` = 512, bounding total item
count independent of byte count — the wire format's cheapest possible item
costs only 4 bytes (`u16` id + `checked` + a zero-length `textLen`), so a
legally-sized 16 KB push could carry thousands of near-empty items. It existed
because the device used to read a stored document back into an in-memory JSON
tree to render it, and an unbounded item count would have made that read-back
unbounded too. That JSON read-back is gone (see "Storage layout" below):
storage and rendering both now walk the exact wire bytes with the same
allocation-free parser that validates the push in the first place, so nothing
downstream cares how a 16 KB document spends its budget. `REJECTED_SIZE`
therefore has one meaning again, not two: over `kMaxListDocLen` bytes.

**`RENDER_STATUS` outcomes for this field:**

```
DISPLAYED       parsed and written through to lists.bin
DECODE_FAILED   malformed — truncated, over-long counts, or a bad `checked` byte
REJECTED_SIZE   over kMaxListDocLen bytes
STORAGE_FAILED  the SD write to lists.bin failed
REJECTED_SHAPE  the pushing peer's declared content shape is not LIST (v12,
                see "UI declaration field")
```

Ingest validates the pushed binary buffer with the same parser that walks it
for rendering (`companiontodo::parseDocument()`) before ever touching SD, then
writes the exact bytes received — no re-encoding — via a temp file renamed
onto `lists.bin` only once that validation passes, so a malformed or over-cap
push never partially overwrites a peer's existing, valid document.

**On-device rendering and navigation are entirely local — no wire traffic.**
Once a document is stored, `Screen::List` shows one list at a time: its title,
its groups (an empty-label group draws no heading), each item's checkbox glyph
reflecting `checked`, and a cursor, paged through a bounded visible window.
The whole document (<=16 KB) is held in RAM for as long as `Screen::List` is
up over it — read once from `lists.bin` on entry, sized exactly to the stored
file via `makeUniqueNoThrow`, and freed the moment the screen is left (Back,
a foreground handover, a live push that takes the screen from underneath a
locally-browsed document) — but only the visible window is ever walked into
rendered rows on any one pass; navigation re-walks that in-RAM buffer with
`companiontodo::parseDocument()`, not SD, on every keypress. This replaced an
earlier design that re-opened and re-parsed `lists.bin` from SD on every
cursor press — a real heap-fragmentation risk on this no-PSRAM part over a
long browse. `Screen::List` claims `Up`/`Down` (move cursor, page the window),
`Left`/`Right` (switch lists within the document) and `Back` (leave the
screen) unconditionally — **not** only what the foreground peer's own button
map left unclaimed, the way the image gallery's `Up`/`Down` paging does. That
is safe specifically because a `LIST` peer's shape, and therefore its need for
all five buttons, is known and enforced before `Screen::List` is ever reached
(see "UI declaration field"). `Confirm` toggles the item under the cursor,
recording a deviation in `list_state.bin` and, if the peer is connected,
announcing it with `LIST_STATE_AVAIL` — see "List-state sync-back" above. The
toggle is purely local otherwise: no phone is required for it, which is the
entire point of the feature.

A newly stored document reaches the screen without a fresh connection: storing
a push fires a device-internal callback naming the peer, and if that peer's
document is the one currently on `Screen::List`, the activity re-reads it from
SD into that in-RAM buffer on the main loop (the new document may be a
different size). This is entirely device-local bookkeeping, not a new wire
message.

**Offline entry point.** `CONFIRM` on a `LIST` peer's tile in the on-screen
icon-grid picker (see "On-screen behaviour" below) opens that peer's stored
document with no phone connected — the same "local SD browse of content the
app already pushed" affordance the image gallery picker already offers, now
serving a second kind of content. This is why content shape is declared in the
persisted UI declaration rather than at `ACQUIRE`: this entry point runs while
disconnected, so `ACQUIRE`-time state could never answer "does this peer show
a list".

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
the device. **23 bytes**, unchanged in layout since v6 — v7 through v11 each
only bumped the version number itself (byte 0), for field `0x04`'s
payload format change, the `HELLO`/UI-declaration additions, the image
`CHUNK` sequence number, the title/body `CHUNK` sequence number, and
`RENDER_STATUS`/`pushId` respectively. v12 additionally sets **two new feature
flag bits** in byte 5; the layout is still 23 bytes. See "v7 changes from
v6" onward:

```
byte 0        protocol version = 12
byte 1        screen width in characters, at the font Companion Mode uses
byte 2        screen height in characters (lines per page)
bytes 3..4    max text field length, uint16 LE — title/body only
byte 5        feature flags: bit0 image, bit1 UI declaration, bit2 icons, bit3 sessions,
              bit4 declared content shape (v12) — the device enforces the UI
              declaration's shape byte, so a client can tell before pushing
              anything that its declaration needs one
              bit5 list-state sync-back (v12) — the device announces and serves
              the on-device check-off diff on the Session characteristic
              (LIST_STATE_AVAIL / LIST_STATE_GET / LIST_STATE)
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
whole reason to guarantee it. A client should:

1. Read the characteristic immediately after connecting.
2. Check byte 0. If it is not the version the client speaks, tell the user
   *"this reader's firmware is too old for this version of <app>"* (or too new)
   and stop. Do not attempt the handshake, and do not guess at the layout — the
   23-byte value shares nothing past byte 4 with v5's 5-byte one.

**Under v12 a stale client no longer gets that far.** `HELLO` carries
`protocolVersion` and the device rejects on strict inequality
(`HELLO_DENIED(PROTOCOL_MISMATCH)`), so byte 0 reading `12` is the check a
client should make, and the handshake is the backstop if it does not. Byte 5
reads `0x3F`: bit 4 says the device enforces the declared content shape, bit 5
that it serves the list-state sync-back conversation.

**This is how a v11 client detects the v12 break before it hits it.** Byte 0
reads 12 and byte 5's bit 4 is set; a client that checks either one knows its UI
declaration needs a content shape byte. A client that checks neither still fails
cleanly rather than mysteriously: its declaration is refused
`ASSET_ACK(REJECTED_NO_SHAPE)`, nothing is stored, and its `ACQUIRE` is then
denied `NO_UI_DECLARATION` — which is already the documented "push field `0x05`,
then retry" path. It will retry with the same shapeless declaration forever, but
it will do so while being told exactly what is wrong, on every attempt.

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

**Not shape-checked (v12).** This is a Status-characteristic write, not a
content-field push, so it is available to a peer of any declared content shape.
Field `0x07` is equally available to a `TEXT` or an `IMAGE` peer — tag state is
an overlay and both shapes permit it (see "Image field" above) — so the choice
between the two routes is about atomicity, not about shape.

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
- **Gallery/list picker** — pressing CONFIRM on the icon grid enters an
  interactive picker over every enrolled peer that either declared
  `IMAGE_GALLERY` **or** declared the `LIST` content shape, most recently
  seen first (not grouped by `appId`, unlike the icon grid: two installs of
  the same app stay two separate tiles, each labelled with `userName` — see
  "Phone → device (write)" above — falling back to `name`). UP/DOWN move the
  cursor; CONFIRM branches on the highlighted peer's declared shape — a
  `LIST` peer opens its stored ToDo List document (`Screen::List`, see "List
  document field" above), any other qualifying peer loads its stored image
  gallery — both a local SD read, the peer need not be currently connected.
  BACK from either returns to this picker, not straight to the icon grid.
  If no peer qualifies at all, CONFIRM on the icon grid shows a brief "No
  photo apps paired yet" message instead of entering an empty picker. If a
  selected image-gallery peer has no stored images, a brief "No images yet"
  message is shown and the picker stays up. None of this involves the phone
  or any wire message — it is firmware-local browsing of content the app
  already pushed (the same "dumb firmware" local browsing as the existing
  UP/DOWN image-gallery navigation described under "UI declaration field"
  above, now covering a second kind of pushed content), just entered a
  different way. It is *not* a launcher: the device cannot and does not
  start anything on the phone.

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
    ui.bin                   UI declaration (content shape + button routing/labels + tag labels)
    lists.bin                 ToDo List document (field 0x08, LIST shape only) -- the exact wire bytes
                              pushed, verbatim, same as ui.bin/icon.bin above, not a JSON re-encoding.
                              Whole-document replace via temp-file-then-rename, never mutated by the
                              device; see "List document field"
    list_state.bin            the on-device check-off diff against lists.bin: a 7-byte header
                              (formatVersion, revision, count) followed by count x { itemId:2, checked:1 },
                              ascending by itemId. Entries are byte-identical to LIST_STATE's wire body,
                              so serving a pull is a header read plus a seek to 7 + 3*offset -- no
                              materialised structure, nothing resident. Cleared whenever a new document
                              lands; see "List-state sync-back"
    data/                    per-peer scratch: staged image, event logs
```

Assets are stored as the exact bytes the phone pushed, digest included — the
digest has to survive verbatim anyway, and a blob costs no parser. `peerKey` is
the first 8 hex chars of a SHA-256 over `appId || installId`. Peer
directories are capped at 32, evicting the least recently seen — unbounded
growth would make `peers.json` unbounded, and it is parsed into RAM.

---

## Version history

### v12 changes from v11 — **breaking**

1. **The UI declaration (`0x05`) gains a mandatory content-shape byte at offset
   4**, immediately after the digest and *before* the button entry count, which
   therefore moves from offset 4 to offset 5. Values: `0x01` TEXT, `0x02` IMAGE,
   `0x03` LIST. `0x00` and anything above `0x03` are invalid, not reserved. See
   "UI declaration field" above for why it sits at the front rather than joining
   the optional trailing bytes, and why it lives here rather than on `ACQUIRE`.
2. **A peer may push only the content fields its declared shape permits.** TEXT:
   title (`0x01`), body (`0x02`), content-id (`0x03`), tag state (`0x07`).
   IMAGE: image (`0x04`), tag state (`0x07`). LIST: the list document (`0x08`)
   and nothing else, not even tag state — see "List document field". Tag
   state is permitted under TEXT and IMAGE because it is an overlay drawn over
   the content, not content itself; LIST has no screen to overlay it onto yet.
   The asset fields, declaration (`0x05`) and icon (`0x06`), are **never**
   shape-checked.
   
   `LIST = 0x03` was reserved by this same v12 change with no content field
   wired to it yet; `kFieldListDoc` (`0x08`) completed that reservation in a
   later change with no version bump — v12 already described the shape,
   `0x08` just gave it something to carry. See "List document field" above
   and `docs/companion-todo-list-design.md`.
3. **New `RENDER_STATUS` result `0x06 REJECTED_SHAPE`.** Latched at the
   offending `START`, before any buffer is allocated, and answered at `END`.
   A whole batch of illegal fields is answered **exactly once**, on its
   final-flagged field, and only when that `END` carried a non-zero `pushId` —
   the same one-answer-per-push contract every other result obeys.
4. **New `ASSET_ACK` result `0x04 REJECTED_NO_SHAPE`.** A declaration with no
   shape byte, or an unknown one, is refused at store time. Nothing is stored,
   so the peer's `ACQUIRE` is then denied by the existing `NO_UI_DECLARATION`
   gate. **No new `ACQUIRE_DENIED` reason was added** — the existing structural
   rule, *a peer that has not declared itself cannot reach the screen*, extends
   to cover shape for free.
5. **Re-pushing the declaration is the only way to change shape**, and it
   triggers a foreground change that clears the screen. Deliberately
   heavyweight; deliberately explicit.
6. **Three new Session-characteristic opcodes** carry the ToDo List check-off
   diff back to the phone: `LIST_STATE_AVAIL` (`0x8B`, device → phone),
   `LIST_STATE_GET` (`0x05`, phone → device) and `LIST_STATE` (`0x8C`,
   device → phone). See "List-state sync-back" above for the layouts and the
   semantics.
7. **No new content field for the sync-back.** `0x09` is still free. It is a
   conversation on the Session characteristic, not a push: the device starts
   it, the phone answers it piecewise, and nothing in it owns the screen —
   none of which `START`/`CHUNK`/`END` framing describes.
8. **Availability notify plus a phone-driven pull, not a single notification.**
   The design this replaces (`docs/companion-todo-list-design.md` §4 as
   originally written) was one notify carrying the whole diff. It cannot
   exist: a realistic shopping list's diff is 150–200 entries × 3 bytes, and
   this characteristic's documented floor is 103 bytes and is **never
   chunked**. Bursting a run of notifies instead would put the whole diff at
   the mercy of NimBLE's finite msys mbuf pool — the same hazard that already
   makes `IMAGE_CHUNK_ACK` fire once per 32 chunks rather than per chunk. The
   pull keeps every message whole and self-describing, so the never-chunked
   invariant holds unchanged; see the note under "Session characteristic".
9. **The sync-back's gate is "has a non-empty diff", not a capability bit.** A
   non-`LIST` peer cannot have a diff — no stored document means no screen to
   toggle anything from — so no client-declared "supports ToDo lists" flag was
   added or is needed.
10. **`list_state.bin`, not `list_state.json`.** The device's diff is stored in
    exactly the byte encoding `LIST_STATE` carries, so a pull is a header read
    and a seek with no materialised structure. See "Storage layout" above and
    `docs/companion-todo-list-design.md` §3.
11. **Capability byte 0 bumped from 11 to 12, and byte 5 gains flag bits 4 and
    5** (declared content shape; list-state sync-back), taking byte 5 from
    `0x0F` to `0x3F`. No capability bytes moved; the block is still 23
    bytes.

**Why breaking, not additive, and why no compatibility path.** A v11 client's
declaration has no shape byte, so under v12 it is refused outright and that
client never reaches the screen; a v12 declaration fed to v11 firmware is worse
than refused, since v11 reads the shape byte as its button count and walks the
rest of the asset off its own layout. An earlier draft of this change proposed a
permissive `LEGACY` value for an absent byte. It was rejected: **every client of
this protocol is written by this project's author** (CompanionKit, SpokenFeeds,
Snap2Ink), so there is no third party to strand, and the only thing a shim would
buy is a permissive mode nobody wants plus a `LEGACY` branch in the firmware
forever. The break is also what makes the shape *useful* — with no legacy peers,
the device always knows the foreground peer's shape, which is what lets it stop
holding buffers a peer cannot use.

**A v11 client fails cleanly, not mysteriously.** In order: it can read
capability byte 0 (`12`) and flag bit 4 before pushing anything and stop;
failing that, its declaration is answered `ASSET_ACK(REJECTED_NO_SHAPE)` — a
result distinct from `REJECTED_FORMAT` precisely so the log says "missing its
shape byte" rather than "malformed"; failing that, its `ACQUIRE` is denied
`NO_UI_DECLARATION`, which is already the documented "push field `0x05`, then
retry" path. At no point does it push content into silence.

**Why the model changed at all.** Through v11 the device was purely reactive:
the last completed push owned the screen, and there was explicitly "no image
mode the client enters or leaves". That was fine for two *passive* shapes —
text and image replace each other harmlessly and re-pushing restores the
previous state exactly. It stops being fine for a shape that holds local user
state (a list's cursor, scroll offset, and not-yet-synced check-offs), which a
stray title/body push from the same app would silently clobber, and for a shape
that claims most of the device's buttons, whose meanings must be known before
the peer's map is applied rather than negotiated per screen. It also cannot
answer the shape question *while disconnected*, which the on-device icon grid
needs. Design record: `docs/companion-declared-shape-design.md`.

### v11 changes from v10 — **breaking**

> **Note on how this section reads.** v11 landed in two steps on the same day,
> before any consumer app had adopted either shape — so what follows describes
> only the shape that actually shipped, not the intermediate one. The first
> pass widened `RENDER_STATUS` with a `field` byte (`0x04` for an image push,
> `0x02` standing in for a whole title/body/content-id/tag batch). That was
> replaced, still within v11, by the `pushId` scheme below before any client
> or script depended on the `field` byte, because a fixed field id does not
> generalize past "image" and "the one text batch field" the way a
> client-chosen, device-echoed id does. The version number was **not** bumped
> a second time for this — see the note at the top of "Status" above.

1. **`IMAGE_STATUS` (`0x88`) is renamed `RENDER_STATUS` and gains a 4th byte,
   `pushId`.** Payload goes from `{opcode, sessionId, result}` to `{opcode,
   sessionId, result, pushId}`. `pushId` is whatever the pushing client sent
   as the third byte of the `END` that triggered this answer — the device
   never interprets it, only echoes it — see "Session characteristic" and
   "Content characteristic" above.
2. **`END` gains a required third byte, `pushId`,** for the same reason: a
   2-byte `END` (the only shape that ever existed before v11) is now
   malformed and rejected. `pushId` 0 means "I am not awaiting a
   `RENDER_STATUS` for this push" and the device never notifies for one. For
   a multi-field atomic batch, only the *final*-flagged field's `pushId` is
   retained as the batch's own id — see "Framing" above.
3. **A title/body/content-id/tag content batch now emits `RENDER_STATUS`
   (`Displayed`, `pushId`) once it actually reaches the panel.** Before v11 a
   text push got no completion signal at all; a client could only guess with a
   fixed delay. Motivated by SpokenFeeds wanting to hold audio playback until
   an article's text is visible: measured on hardware, a text push completes
   on the wire in ~0.24s but the panel's own settle takes a further ~2.2s.
4. **A content batch discarded whole because one of its fields hit
   `FIELD_SEQ_GAP` now also emits `RENDER_STATUS(SEQUENCE_GAP, pushId)`,**
   immediately rather than leaving the client to wait out its own timeout for
   a render that was never going to happen — see "Atomic multi-field pushes".
5. **Capability byte 0 bumped from 10 to 11.** No other capability bytes
   moved.

Why breaking, not additive: a v10 client parses `IMAGE_STATUS` as a fixed
3-byte payload and only expects it in response to an image push, and parses
`END` as a fixed 2-byte frame. Under v11 a text push also triggers
`RENDER_STATUS`, every occurrence — image or text — now carries an extra
trailing byte, and every `END` on the wire carries one more byte than it used
to. A v10 client (or a v11-`field`-byte client, had one shipped) fed any of
these changes would misparse the notification, misattribute a text push's
answer to an image push it never made (see `CompanionClient.swift`'s
`pendingRenders`, keyed by `pushId` for exactly this reason), or send a
now-malformed `END`. Reusing `0x88` rather than adding a second opcode was a
deliberate choice: the receiver's job — "was this thing on screen" — is
identical for both, and a second opcode would have meant a second,
near-duplicate implementation on both sides for no behavioural gain.

### v10 changes from v9 — **breaking**

1. **The title (`0x01`) and body (`0x02`) fields' `CHUNK`s gain a 2-byte
   little-endian sequence number**, the same framing the image field got in
   v9 — see "Framing" and "Title/body fields" above. Content-id, UI
   declaration, icon and tag state are unchanged.
2. **Title and body `CHUNK`s are now pushed over Write Without Response**
   (recommended, not enforced at the GATT level). `START` and `END` stay
   Write. See "Atomic multi-field pushes" above for the measurement behind
   this and why the sequence number exists.
3. **New device → phone notification `FIELD_SEQ_GAP` (`0x8A`)** — the device
   detected a title/body CHUNK sequence number that skipped ahead of what it
   expected and dropped the field rather than rendering it. See "Session
   characteristic" and "Title/body fields" above. Unlike the image field's
   `SEQUENCE_GAP`, there is no periodic mid-transfer ack for title/body — the
   push is too short for one to be worth the wire traffic.
4. **Capability byte 0 bumped from 9 to 10.** No other capability bytes moved.

Why: the same root cause as v9, on a field that gets pushed far more often.
Measured on real hardware, a write-with-response round trip on the Content
characteristic costs ~120ms regardless of connection interval — the
peripheral's own `onWrite` handling is 0-1ms, so the round trip itself is the
floor. For a large image that floors a bulk transfer; for title/body it
showed up differently — a consumer app (SpokenFeeds) pushing a full page of
article text every few seconds started visibly falling behind, because each
push had to clear its own several-chunk write-with-response tail before the
next one could start, on top of the busy/idle connection-interval
renegotiation (see `docs/companion-display-protocol.md`'s adaptive
connection-interval notes in `src/CompanionBle.cpp`) that a short gap between
pushes can also trigger. Write Without Response removes the per-chunk round
trip the same way it did for images, at the same cost (BLE's own delivery
guarantee), covered the same way (a sequence number, checked device-side).
There is no partial-resume protocol: recovering from `FIELD_SEQ_GAP` means
re-pushing the field from a fresh `START`.

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
`ACQUIRE_DENIED` / `ASSET_ACK` / `RENDER_STATUS` notifications, `sessionId` on
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
  declaration, icons, ToDo List document storage as verbatim wire bytes);
  `src/CompanionTodoDocument.{h,cpp}` (the host-buildable, allocation-free wire
  parser for field `0x08`, used for both validating a push and walking a
  stored document back out — see "List document field" for why there is no
  separate JSON codec here anymore), `src/CompanionTodoNav.{h,cpp}`
  (host-buildable cursor/paging/list-switching state machine); `src/activities/companion/CompanionModeActivity.{h,cpp}` (the
  on-device screen: pagination, pairing prompt, button routing, image render,
  sleep grid, `Screen::List`). See `docs/companion-mode-implementation-notes.md` for the
  bring-up log.
- **Swift client**: `CompanionKit` (https://github.com/DarkStarDS9/CompanionKit)
  — a SwiftPM package implementing discovery, the handshake,
  token/appId/installId persistence, ACQUIRE/RELEASE, asset digest
  compare-and-push, the framer, and button-event decoding. Shared by this
  fork's consumer apps via SPM; see its `README.md` and `clients/README.md`
  here.
- **Python client**: `scripts/companion_protocol.py` is the single Python
  implementation of everything above — UUIDs, opcodes, field ids, the
  notification decoder, capability parsing, the handshake, token persistence
  and the START/CHUNK/END framer. `scripts/push_companion_content.py` (pushes
  title/body/content-id, a UI declaration, an icon and an image straight from a
  dev machine — run with `--help`) and `scripts/companion_e2e_test.py` both
  import it and carry no wire format of their own. Needs `bleak`; see
  `scripts/requirements.txt`. A protocol change has exactly one Python site to
  edit, which is deliberate: the two scripts used to carry a copy each, and one
  of them silently froze at v6 for five protocol versions.
- **On-target test harness**: `scripts/companion_e2e_test.py` drives BLE and
  injected button presses together against a `[env:test]` build and asserts on
  enrollment, reconnect, `ACQUIRE` gating, atomic content batches and their
  `RENDER_STATUS`, both sequence-gap paths, tags, preemption between two apps on
  one link, and a full-screen image push diffed pixel-for-pixel against
  `CMD:SCREENSHOT`. Its `[shape]` group covers v12: the two enrolled peers now
  carry *different* declared shapes (a TEXT peer and an IMAGE peer, since one
  peer pushing both is no longer a legal client), and it asserts
  `REJECTED_SHAPE` in both directions — including that a three-field batch to
  the wrong-shaped peer produces **exactly one** answer, not one per field —
  plus `REJECTED_NO_SHAPE` on a shapeless declaration with the `ACQUIRE` denial
  that follows it, and the re-declare-while-foreground escape hatch. Its
  `[list]` group covers the ToDo List document field (`0x08`): a successful
  push and render, the size refusal (`kMaxListDocLen`), and a malformed
  document — like `[shape]`, **written but not executed against a device**,
  and deliberately not folded into `[shape]` since it needs a third declared
  peer and a field `[shape]` knows nothing about.
  See `docs/companion-test-console.md`. CompanionKit's framer
  and handshake codec additionally have `swift test` unit tests; the rest of
  what runs on-device is verified by the checklist below. The host-only
  parser and nav-state seams (`test/companion_todo_document/`,
  `test/companion_todo_nav/`) are the one part of the ToDo List feature that
  has actually run, since they need no hardware at all.

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
11b. **(v12)** From a peer that has never stored a declaration, push field `0x05`
    with the shape byte omitted (a v11-shaped declaration): confirm
    `ASSET_ACK(REJECTED_NO_SHAPE)`, that `CMD:CUI` still reports no stored
    declaration for that peer, and that `ACQUIRE` is then denied
    `NO_UI_DECLARATION`. Repeat with a shape byte of `0x00` and of `0x04`:
    both are refused the same way, not tolerated.
11c. **(v12)** Push a valid declaration and confirm `CMD:CUI` reads back
    `shape=` alongside the button count — the shape byte sits in front of that
    count, so a device reading one back correctly also proves it did not mistake
    it for the other.
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
    for the byte layout), with a non-zero `pushId` on `END`, using only sample
    values `{0, 1, 2, 3}`: confirm it renders full-screen, the grayscale
    settle runs, and `RENDER_STATUS(DISPLAYED, pushId)` arrives echoing that
    same id.
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
25. Push garbage bytes as field `0x04` with a non-zero `pushId`:
    `RENDER_STATUS(DECODE_FAILED, pushId)` and the previous screen is retained.
    Also push one with `pushId` 0 and confirm no `RENDER_STATUS` arrives at
    all.
26. Disconnect mid-image: confirm the partial staged file is discarded and the
    device does not try to decode it.
27. Push an image larger than the advertised max:
    `RENDER_STATUS(REJECTED_SIZE, pushId)`.
27b. **(v11)** Send a 2-byte `END` (no `pushId`) on any field: confirm the
    device logs a rejection and the field is dropped rather than tolerated —
    see "Framing" above.
28. **(v12; inverts the pre-v12 test, which was "push a body after an image and
    confirm the screen returns to text")** Push a body to an `IMAGE`-declared
    peer with a non-zero `pushId`: confirm `RENDER_STATUS(REJECTED_SHAPE,
    pushId)` and that the image is still on screen. Then push the same body as
    a three-field batch (title, body, final-flagged content-id) and confirm
    **exactly one** `RENDER_STATUS` arrives for the batch, not one per field.
28a. **(v12)** Push an image to a `TEXT`-declared peer with a non-zero `pushId`:
    confirm `RENDER_STATUS(REJECTED_SHAPE, pushId)` and that the text page is
    untouched. Then re-push that peer's UI declaration with shape `IMAGE`,
    confirm the screen clears (a foreground change), and confirm the same image
    is now accepted and renders.
28b. With a tag visible, push an image and confirm the chip is drawn over the
    print; hide every tag, re-push, and confirm the print is untouched. **(v12)**
    Do it both ways round: a **Status characteristic** write (not shape-checked)
    and a field `0x07` push batched with the image (permitted, since tag state
    is an overlay rather than content) — an `IMAGE` peer must be accepted on
    both routes, with the batched one committing image and chip in one redraw.
28c. **(v9)** Push an image over Write Without Response with correctly
    incrementing CHUNK sequence numbers: confirm `IMAGE_CHUNK_ACK` notifies
    roughly every 32 chunks and the transfer still ends in
    `RENDER_STATUS(DISPLAYED, pushId)`. Then push one with a deliberately
    skipped sequence number: confirm `RENDER_STATUS(SEQUENCE_GAP, pushId)`
    and that the previous screen is retained, the same as a decode failure.
28d. **(v10)** Push title+body over Write Without Response with correctly
    incrementing CHUNK sequence numbers: confirm the page renders normally
    and no `FIELD_SEQ_GAP` arrives. Then push one with a deliberately skipped
    sequence number: confirm `FIELD_SEQ_GAP` for the affected field and that
    the previous screen is retained rather than a spliced/corrupted page.
    Also push a rapid sequence of short title+body updates (a few seconds
    apart, full page each) and confirm the on-screen text keeps pace instead
    of visibly lagging behind — the scenario that motivated this change.
28e. Push an atomic batch (title, body, final-flagged content-id) with a
    deliberately skipped sequence number in the **title** only: confirm
    `FIELD_SEQ_GAP` names the title and that the screen still shows the
    *previous* article's title AND body — not the new body under the old
    headline. Repeat with the gap in the body. Repeat once more without
    sending the final-flagged field at all, and confirm the batch is still
    discarded (not applied) when the 3 s timeout fires.
28f. Push an atomic batch of title + body + tag state (`0x07`) with the gap in
    the title: confirm no tag chip changes on screen — the previous article
    must keep its own tags, not inherit the new one's. Then toggle a single
    tag on its own (a standalone `0x07` push, no title/body) and confirm it
    still applies immediately.
28g. **(v11)** Push title+body (final-flagged body) with a non-zero `pushId`
    on the final field's `END`, and time the gap between that `END`'s write
    completing and `RENDER_STATUS(Displayed, pushId)` arriving, confirming the
    id matches: expect roughly the ~2.2s settle measured on hardware, not an
    immediate reply. While a page is on screen, turn a page locally (a button
    press, no new push) and confirm no `RENDER_STATUS` fires — only a push
    gets an answer. Repeat with `pushId` 0 on the final field and confirm no
    `RENDER_STATUS` arrives even once the settle finishes. Then repeat the
    sequence-gap test from 28d/28e (with a non-zero `pushId`) and confirm
    `RENDER_STATUS(SequenceGap, pushId)` arrives immediately (not after the
    settle) alongside the existing `FIELD_SEQ_GAP`, since the batch never
    renders at all — and confirm the id echoed is the one from the batch's
    *final*-flagged field's `END`, not an earlier field's, by giving an
    earlier field a different `pushId`.

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
