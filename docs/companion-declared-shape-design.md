# Declared Content Shape — Design

**STATUS: §9 slice A is implemented in firmware, protocol doc and host harness; nothing has run on
hardware.** The authoritative wire contract is `docs/companion-display-protocol.md`, now at v12 and
describing the model below rather than the opposite of it. Landed: the mandatory shape byte, the
ACQUIRE-time cache, the START-latch/END-answer refusal, `RejectedShape` and `RejectedNoShape`,
version 12 and capability bit 4, host unit tests over the parse and the permitted-field table, and
the harness's `[shape]` group. **Not** landed and not claimed: no part of this has been exercised
against a real device — the harness cases are written but unexecuted — and slices B, C and D (the
RAM savings, and the CompanionKit/consumer-app releases) are untouched.

One deliberate amendment since this was written: **tag state (`0x07`) is permitted under both `TEXT`
and `IMAGE`**, because it is an overlay rather than content — see §5.

Prompted by `docs/companion-todo-list-design.md`: adding a third content shape exposed that the
first two were never really "modes" at all.

---

## 1. What is implemented today, and how it differs from what was intended

The intent, as understood by this project's author, was: *an app chooses and initialises the mode it
wants at handshake, and cannot switch while connected; its button definitions go with that mode.*

**Half of that is true.** The UI declaration (`kFieldUiDeclaration`, `0x05`) is per-peer, mandatory,
persisted, versioned by an opaque tag, and it **gates `ACQUIRE`** — a peer that has not declared what
its buttons do cannot reach the screen (`src/CompanionBle.cpp:980-983`). Buttons genuinely are
declared up front and fixed for the session.

**The content shape half is not.** There is no shape field anywhere in the handshake, and nothing
enforces one:

- `HELLO` carries `appId`, `installId`, token, name, `userName`, `helloTag` — no shape
  (`src/CompanionBle.cpp:881`). `ACQUIRE` is two bytes, and validates only that the session exists
  and a UI declaration is stored (`src/CompanionBle.cpp:970-987`).
- `screen = Screen::Text` fires unconditionally whenever a title/body batch commits
  (`src/activities/companion/CompanionModeActivity.cpp:1209`); `screen = Screen::Image` whenever an
  image push completes (`:753`). Neither consults anything declared.
- `kUiCapabilityImageGallery` (`src/CompanionBle.h:189`) looks like a shape declaration but is not:
  it is never consulted on the image ingest path, only for whether the peer gets a tile in the
  on-device gallery picker (`CompanionModeActivity.cpp:859`, `:982`).
- The protocol doc says so explicitly (`docs/companion-display-protocol.md:924-928`): *"There is no
  compositing of the two, and no 'image mode' the client enters or leaves: the last completed push of
  either kind is what is on screen."*

So the current model is **purely reactive: the last completed push owns the screen.**

## 2. Why reactive was fine for two shapes and is not fine for three

Reactive cost nothing for text and image because **both are passive renderings**. Nothing is lost
when one replaces the other; re-pushing restores the previous state exactly. That is why the "no
image mode" line above was a reasonable simplification rather than an oversight.

`Screen::List` breaks both properties:

- **It holds user state the phone does not have.** A cursor, a scroll offset, and — critically —
  pending local check-offs in `list_state.json` that have not synced yet
  (`docs/companion-todo-list-design.md` §3, §5). Under reactive rules, the same app pushing a
  "Sync complete" title/body silently replaces the list mid-shopping-trip. Text and image have no
  equivalent failure.
- **It claims most of the device's buttons.** The gallery gets away with local Up/Down navigation by
  only claiming buttons the peer's own map left unclaimed
  (`docs/companion-multi-app-design.md:96-106`). A list needs Up/Down/Left/Right/Confirm — that
  "only take what's unclaimed" trick does not scale to it, so the list's button meanings have to be
  known to be safe *before* the peer's map is applied, not negotiated per screen.

## 3. Decision: the shape is declared in the UI declaration, not on `ACQUIRE`

Both were considered. **The UI declaration wins, decisively, for one reason: the todo list must work
with no phone connected.**

Its primary entry point is CONFIRM on the peer's tile on the idle icon grid, opening that peer's
stored document with no session in existence (`docs/companion-todo-list-design.md` §4). The device
therefore has to know a peer's shape **while disconnected**. An `ACQUIRE`-time byte is per-session
state and cannot answer that question; the UI declaration is already persisted per-peer on SD and
already read while disconnected for exactly this class of decision (`isImageCapable()` drives the
gallery picker today, `src/CompanionPeerStore.cpp:401-412`).

Secondary reasons, all pointing the same way:

- It is already mandatory and already gates `ACQUIRE`, so "a peer with no declared shape" is an
  impossible state rather than a case to handle — the same structural argument
  `docs/companion-multi-app-design.md` §7 makes for the button map.
- It is already versioned by an opaque tag, so an app that changes shape in an update gets picked up
  on the next connect with no re-pairing, via machinery that already exists.
- Shape and buttons belong together: a list peer's Up/Down/Confirm meanings are a consequence of
  being a list peer. Declaring them in one asset is coherent, and halves the digest bookkeeping,
  which is the same argument that folded button labels and tag labels into one asset to begin with
  (`src/CompanionBle.h:199-202`).
- It is cheap *to specify*, but not as cheap to implement as an earlier draft of this bullet claimed.
  That draft said the change was `return len - offset <= 2;` becoming `<= 3`
  (`src/CompanionPeerStore.cpp:163`) plus one read in `loadUiDeclaration()`. Both are wrong: under
  the offset-4 placement above, the *trailing* byte count does not change at all, and there turned
  out to be **four** independent hand-rolled walks of this layout, not two —
  `uiDeclarationParses()`, `isImageCapable()` (`CompanionPeerStore.cpp:401`),
  `CompanionModeActivity::loadUiDeclaration()`, and the `CUI` command in `CompanionTestConsole.cpp`.
  Every one of them starts its walk at a hardcoded offset. Inserting a byte at offset 4 breaks all
  four silently — the firmware still builds and every host test still passes, because three of the
  four are hardware-only code. This is the real cost of the change and the reason the parse was
  extracted into `src/CompanionUiDeclaration.{h,cpp}` with a shared `kBodyFirstButtonOffset` rather
  than patched in place.

### Wire shape

**A mandatory byte immediately after the 4-byte digest, before the button count** — *not* a third
trailing byte after `TagRenderStyle` and the capability bitmask, which is where an earlier draft put
it. That placement is unimplementable: the declaration's trailing fields signal absence by the buffer
running out (`src/CompanionPeerStore.cpp:158-163`), so a **mandatory** field cannot sit behind
optional ones — given a single trailing byte, a decoder cannot tell a shape from a render style.
Inserting at offset 4 is free precisely because §6 is a clean break, and it leaves the
trailing-optional convention intact for the two fields that legitimately use it.

```
bytes 0..3   opaque digest
byte  4      content shape              <- new, mandatory
byte  5      button entry count N
N x { buttonId:1, routing:1, labelLen:1, label[labelLen] }
byte         tag entry count M          (optional; absent means zero tags)
M x { tagId:1, labelLen:1, label[labelLen] }
byte         tag render style           (optional; absent means Bordered)
byte         capabilities bitmask       (optional; absent means none)
```

The shape values:

```
0x01  TEXT     title/body/content-id/tag-state
0x02  IMAGE    image, tag-state (an overlay, permitted under both -- see §5)
0x03  LIST     todo-list document
```

`0x00` and anything above `0x03` are invalid, not reserved — an unknown shape is refused rather than
tolerated, since the whole point is that the firmware always knows what a peer will push.

**Mandatory** — there is no unset value and no default. A declaration without the byte fails
`uiDeclarationParses()` and is rejected at store time, which means the peer has no stored declaration
at all, which means `ACQUIRE` is already refused by the existing `kAcquireDeniedNoButtonMap` gate
(`src/CompanionBle.cpp:980-983`). No new `ACQUIRE_DENIED` reason is needed: the existing structural
rule — *a peer that has not declared itself cannot reach the screen* — extends to cover shape for
free, exactly as `docs/companion-multi-app-design.md` §7 argues for the button map.

The rejection is reported on `ASSET_ACK` with a distinct `AssetStoreResult`:

```
RejectedNoShape = 0x04   declaration carries no content shape
```

Distinct from the generic `RejectedFormat` on purpose. It costs one enum value and is worth it
precisely during the migration in §6, when three codebases are being updated at once and "your
declaration is missing its shape byte" is a far better thing to read in a log than "malformed".

A single value, not a bitmask. The bitmask option (declare a *set* of permitted shapes, allocate
their union) was rejected: it solves the RAM question but not the two problems in §2, because a peer
declaring `TEXT|LIST` reintroduces exactly the state-clobbering and button-claiming ambiguity the
declaration exists to remove.

## 4. Switching shape is possible, but only deliberately

"Cannot switch while connected" is the default and the enforced behaviour. There is, however, already
a sanctioned escape hatch that costs no new wire surface: **re-pushing the UI declaration while
foreground already triggers a foreground-change** (`src/CompanionBle.cpp:1061-1066`), which reloads
the button map and clears the screen (`CompanionModeActivity.cpp:670-701`).

That is exactly the right cost model. Changing shape becomes an explicit, heavyweight, screen-clearing
act — an app re-declaring what it is — rather than an implicit consequence of which field it happened
to push. An app that genuinely needs both shapes (a reader that wants to show one hero image) can do
it; it just cannot do it by accident, and the firmware always knows the current answer without
inspecting content.

## 5. Enforcement, and the hot-path trap

**The trap:** at content-push time the peer's declaration is *not* in RAM. `Session` carries only
`peerKey`/`contentId`, and every existing declaration read is an SD read (`hasUiDeclaration()` is a
`Storage.exists()`, `CompanionPeerStore.cpp:274`; `isImageCapable()` is a full open/read/parse,
`:401-412`). Checking shape per push would put an SD open on the NimBLE host task for **every field
of every push** — precisely what the image writer task exists to avoid
(`src/CompanionBle.cpp:605-640`, `:737-747`). Any implementation that does this is wrong, however correct it
looks.

**The fix:** cache the declared shape once, at `ACQUIRE`, where the declaration is already being read
— as implemented, that read is `companionpeer::readDeclaredShape()` (`src/CompanionBle.cpp:1014-1039`,
which also tightened the old bare `hasUiDeclaration()` existence check into a full parse). A byte on
`Session` (`declaredShape`), set in the `ACQUIRE` handler just before `setForeground()` (`:383-399`)
and refreshed in `finishAsset()`'s declaration-re-push branch (`:1109-1133`, the read at `:1127`) so
§4's escape hatch stays correct. That keeps the ACQUIRE-time read as the only SD touch.

**Tag state is exempt: it is an overlay, not content.** Field `0x07` is permitted under *both* `TEXT`
and `IMAGE` (`src/CompanionUiDeclaration.cpp:90-98`). A tag is a chip drawn over whatever content is
on screen, so it fails this design's own §2 test for what a shape is for — it holds no screen state
the phone lacks and claims no buttons — and refusing it would un-design the protocol's atomic
image+tag push, reinstating the accept-then-never-draw no-op that
`docs/companion-display-protocol.md`'s "Tags are drawn over an image" section says is gone. `LIST`
still permits nothing, `0x07` included: there is no list screen to overlay yet, and forward-dating
that is unproven.

**Rejection path:** reject at `START`, before any buffer is allocated — the natural slot is right
after the `isKnownField` + `sessionById` checks — as implemented, `src/CompanionBle.cpp:1195-1208`,
just past those checks at `:1152-1157`. `START` carries no `pushId` (it arrives only on `END`,
`:1396`), so this must **latch at START and answer at END**, mirroring `g_activeImageOverflow`
(`:432`, latched `:1213`, answered `:1420`) exactly rather than inventing a new pattern; the latch is
`g_activeShapeRejected` (`:439`) and the answer is at `:1401-1408`.
The answer is a new `RenderResult` case:

```
RejectedShape = 0x06   this peer declared a different content shape
```

## 6. Clean break, v11 → v12

**Decided: a clean break, no legacy path.** An earlier draft of this document proposed treating an
absent shape byte as a permissive `LEGACY` value, reasoning from `docs/companion-multi-app-design.md`
§9's record of what v6's coordinated release cost. That reasoning does not apply here: **every client
of this protocol is written by this project's author.** There is no third-party consumer to strand,
so the only thing a compatibility shim would buy is the right to keep a permissive mode nobody wants,
in exchange for a `LEGACY` branch living in the firmware forever.

Consequences, stated plainly rather than discovered later:

- **The shipped SpokenFeeds and Snap2Ink builds stop working against v12 firmware** until each ships
  a build that declares a shape. Same coordination shape as v6 — firmware and clients release
  together. Both are one-line changes (each app is already single-shape in practice: SpokenFeeds
  pushes only title/body, `SpokenFeedsMixer/Services/CompanionDeviceService.swift:288`; Snap2Ink only
  images, `Snap2Ink/Transport/CompanionKitTransport.swift:87`).
- **A v11 client against v12 firmware fails cleanly, not mysteriously**: its declaration is refused
  with `RejectedNoShape` on `ASSET_ACK`, and its subsequent `ACQUIRE` is denied
  `NO_UI_DECLARATION` — which is already the documented "push field `0x05`, then retry" path
  (`docs/companion-display-protocol.md:271`). The capability characteristic also reports version 12,
  so a client can detect the mismatch before pushing anything.
- **The RAM payoff (§7) becomes unconditional** rather than opt-in per app. This is the substantive
  win of breaking: with no legacy peers, the firmware always knows the foreground shape, so the
  buffers in §7 are genuinely never allocated for a peer that cannot use them — instead of being
  kept resident against the possibility of a legacy peer that might.

Version bump **11 → 12**. Feature bitmask bit 4 is free for shape-awareness (`src/CompanionBle.cpp:825`,
currently `0x0F`); the version literal at `src/CompanionBle.cpp:830` is a bare `11` and should become
a named constant in the same change. `scripts/companion_protocol.py:46` (`PROTOCOL_VERSION = 11`) and
CompanionKit's major version (which tracks the protocol version by convention) move together.

## 7. What declaring actually saves

| Declared shape | RAM it no longer needs | Where |
|---|---|---|
| `IMAGE` or `LIST` | **~8 KB** — `titleBuf_[4096]` + `bodyBuf_[4096]` | `src/CompanionBatchModel.h:109,113`, resident via file-scope `g_batchModel`, `CompanionModeActivity.cpp:102` |
| `TEXT` or `LIST` | **~9.2 KB** — image writer queue (`ImageWorkMsg` × 10 ≈ 5.2 KB) + writer task stack (4 KB) | `src/CompanionBle.cpp:578-589`, `:608`, `:710-720` |

Nothing else is both shape-specific and sizeable — image pushes already stream to SD with no large
RAM buffer, and decode is row-at-a-time
(`lib/Epub/Epub/converters/RawBitmapToFramebufferConverter.cpp:82-96`).

Because §6 is a clean break, **every peer declares, so these savings are unconditional** — there is
no legacy peer whose possible text push forces the 8 KB to stay resident just in case. That is the
concrete return on breaking rather than shimming.

**This is what makes the allocation question fall out for free.** `g_batchModel`'s fixed 8 KB is a
vestige of the first Companion Mode commit (`59c7d937`), when title/body was the only shape — it
predates image mode entirely (v6, `a4f2fcea`). With a declared shape the firmware knows at foreground
time what to allocate, so this becomes an allocation in `applyForegroundChange()`
(`CompanionModeActivity.cpp:670-701`, with the empty-`peerKey` branch at `:674-678` as the free
point) — **on the main loop task, once per foreground change**, not on the BLE host task and not per
push. That is a materially safer shape than the lazy first-touch allocation considered before this
design existed, and it is why that idea should not be pursued separately.

Gating the image writer task's creation (`src/CompanionBle.cpp:710-720`) is the larger half and can
land after the text half; it is called out here so the saving is not double-counted as free.

## 8. What this breaks, stated plainly

**Every client, until each declares a shape** — see §6. That is accepted, not regretted: all of them
are this project's own. Concretely, a coordinated release of firmware v12, CompanionKit 12.x,
SpokenFeeds and Snap2Ink.

Plus four things we own inside this repo and must change knowingly (and one thing this deliberately
does **not** break: tag state, field `0x07`, stays legal for every content shape but `LIST`, because
it is an overlay drawn over the content rather than content of its own — see §5):

- **`docs/companion-display-protocol.md:924-928`** — the "no image mode the client enters or leaves"
  paragraph is directly contradicted and must be rewritten, not amended.
- **Manual test 28 (`:1769`)** — *"Push a body after an image and confirm the screen returns to
  text."* No longer a valid behaviour at all under a clean break; it inverts into its opposite —
  push a body to an `IMAGE`-declared peer and confirm it is **refused** with `RejectedShape`.
- **`docs/companion-display-protocol.md:477-478`** — the `pushId` rationale cites running "an image
  and a text batch" concurrently as the motivating example. `pushId` remains correct and necessary
  (two concurrent *same-shape* pushes, and the general "one answer per push" contract), but that
  example must be replaced.
- **`scripts/companion_e2e_test.py`** — `session_a` pushes title/body (`:799-800`) and later an image
  (`:1192`) on the same enrolled peer. That is now an illegal client, so it must be split into two
  peers with different declared shapes, and grow new cases asserting `RejectedShape` and
  `RejectedNoShape` both fire, per this repo's every-fix-starts-red rule.

## 9. Sequencing

- **A — Declaration + enforcement.** §3, §5, §6. The wire change: the shape byte, the ACQUIRE-time
  cache, the START-latch/END-answer rejection, `RejectedShape`, version 12, capability bit 4. Host
  unit tests cover the parse (`test/`), the harness covers rejection.
- **B — Text-buffer allocation.** §7's first row: `g_batchModel` moves behind the declared shape,
  allocated in `applyForegroundChange()`. Needs A. This is the change that retires the 8 KB vestige.
- **C — Image writer gating.** §7's second row. Independent of B; larger blast radius since it
  touches `ensureStarted()`. Can slip.
- **D — CompanionKit + consumer apps.** Declare a shape in `UiDeclaration` (`TEXT` for SpokenFeeds,
  `IMAGE` for Snap2Ink), bump CompanionKit's major to 12 per its protocol-tracking convention, bump
  each app's pin. **Ships with A, not after it** — under §6's clean break, v12 firmware and a v11
  client cannot both be in the field working. This is the coordination cost the break buys, and it is
  the one part of this plan that is not just a firmware change.

Only A blocks `docs/companion-todo-list-design.md` phase A. A and D block each other's *release*, not
each other's *development* — A is provable on its own against the host harness before any app moves.
