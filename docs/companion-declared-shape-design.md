# Declared Content Shape — Design

**STATUS: proposal, not implemented.** The authoritative wire contract remains
`docs/companion-display-protocol.md` (v11), which today describes the **opposite** of this document.
Nothing here is live until that doc is updated in the same commit as the firmware, per this repo's
rule that the protocol doc is the product.

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
gallery picker today, `src/CompanionPeerStore.cpp:426-450`).

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
- It is cheap. `uiDeclarationParses()` currently ends `return len - offset <= 2;`
  (`src/CompanionPeerStore.cpp:163`) — permitting at most two trailing optional bytes. One more byte
  means `<= 3` there and one more read in `loadUiDeclaration()`
  (`CompanionModeActivity.cpp:355-361`), following the established "ran out of buffer means absent"
  convention.

### Wire shape

A third optional trailing byte on the UI declaration, after the `TagRenderStyle` byte and the
capability bitmask:

```
0x00  LEGACY   unset/absent — text and image both permitted, list forbidden (see §6)
0x01  TEXT     title/body/content-id/tag-state
0x02  IMAGE    image
0x03  LIST     todo-list document
```

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
`Storage.exists()`, `CompanionPeerStore.cpp:299`; `isImageCapable()` is a full open/seek/read,
`:426-450`). Checking shape per push would put an SD open on the NimBLE host task for **every field
of every push** — precisely what the image writer task exists to avoid
(`src/CompanionBle.cpp:600-608`). Any implementation that does this is wrong, however correct it
looks.

**The fix:** cache the declared shape once, at `ACQUIRE`, where the declaration is already being read
(`hasUiDeclaration()` at `src/CompanionBle.cpp:980`). A byte on `Session` set in `setForeground()`
(`:363-379`), refreshed in `finishAsset()`'s declaration-re-push branch (`:1061`) so §4's escape
hatch stays correct. That keeps the ACQUIRE-time read as the only SD touch.

**Rejection path:** reject at `START`, before any buffer is allocated — the natural slot is right
after the `isKnownField` + `sessionById` checks (`src/CompanionBle.cpp:1105-1108`). `START` carries no
`pushId` (it arrives only on `END`, `:1297`), so this must **latch at START and answer at END**,
mirroring `g_activeImageOverflow` (`:1123` → `:1310`) exactly rather than inventing a new pattern.
The answer is a new `RenderResult` case:

```
RejectedShape = 0x06   this peer declared a different content shape
```

## 6. Backward compatibility: additive, not a v6-style clean break

An absent shape byte means `LEGACY`: text and image both permitted, list forbidden, current
behaviour exactly preserved.

This is deliberate and is the one place this design does **not** follow the author's stated intent to
the letter — a legacy peer can still switch between text and image mid-session. It is worth it:

- `docs/companion-multi-app-design.md` §9 records what v6's clean break cost — *"flashing v6 firmware
  breaks the shipped SpokenFeeds build until it ships a v6 update. Both sides must be released
  together."* That coordination cost is real and was paid once already.
- It buys nothing here. Both shipped apps are already single-shape **in practice** (SpokenFeeds
  pushes only title/body, `SpokenFeedsMixer/Services/CompanionDeviceService.swift:288`; Snap2Ink only
  images, `Snap2Ink/Transport/CompanionKitTransport.swift:87`), so forcing a break would break them
  to enforce a rule they already follow.
- Adoption is then opt-in per app, and the RAM payoff (§7) lands per app as each declares.

Version bump **11 → 12** regardless, since the capability characteristic must advertise
shape-awareness so a client can tell whether declaring is even understood: feature bitmask bit 4 is
free (`src/CompanionBle.cpp:825`, currently `0x0F`). The version literal at `src/CompanionBle.cpp:830`
is a bare `11` and should become a named constant in the same change.

## 7. What declaring actually saves

| Declared shape | RAM it no longer needs | Where |
|---|---|---|
| `IMAGE` or `LIST` | **~8 KB** — `titleBuf_[4096]` + `bodyBuf_[4096]` | `src/CompanionBatchModel.h:109,113`, resident via file-scope `g_batchModel`, `CompanionModeActivity.cpp:102` |
| `TEXT` or `LIST` | **~9.2 KB** — image writer queue (`ImageWorkMsg` × 10 ≈ 5.2 KB) + writer task stack (4 KB) | `src/CompanionBle.cpp:578-589`, `:608`, `:710-720` |

Nothing else is both shape-specific and sizeable — image pushes already stream to SD with no large
RAM buffer, and decode is row-at-a-time
(`lib/Epub/Epub/converters/RawBitmapToFramebufferConverter.cpp:82-96`).

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

Nothing in the field (§6), but three things we own and must change knowingly:

- **`docs/companion-display-protocol.md:924-928`** — the "no image mode the client enters or leaves"
  paragraph is directly contradicted and must be rewritten, not amended.
- **Manual test 28 (`:1769`)** — *"Push a body after an image and confirm the screen returns to
  text."* Still valid for a `LEGACY` peer; must be re-scoped to say so.
- **`docs/companion-display-protocol.md:477-478`** — the `pushId` rationale cites running "an image
  and a text batch" concurrently as the motivating example. `pushId` remains correct and necessary
  (two concurrent *same-shape* pushes, and the general "one answer per push" contract), but that
  example must be replaced.
- **`scripts/companion_e2e_test.py`** — `session_a` pushes title/body (`:799-800`) and later an image
  (`:1192`) on the same enrolled peer. Under a declared shape this becomes either a `LEGACY`-peer
  test or two peers; it must also grow a new case asserting `RejectedShape` fires, per this repo's
  every-fix-starts-red rule.

## 9. Sequencing

- **A — Declaration + enforcement.** §3, §5, §6. The wire change: the shape byte, the ACQUIRE-time
  cache, the START-latch/END-answer rejection, `RejectedShape`, version 12, capability bit 4. Host
  unit tests cover the parse (`test/`), the harness covers rejection.
- **B — Text-buffer allocation.** §7's first row: `g_batchModel` moves behind the declared shape,
  allocated in `applyForegroundChange()`. Needs A. This is the change that retires the 8 KB vestige.
- **C — Image writer gating.** §7's second row. Independent of B; larger blast radius since it
  touches `ensureStarted()`. Can slip.
- **D — CompanionKit + consumer apps.** Declare a shape in `UiDeclaration`; opt-in per app, so
  SpokenFeeds and Snap2Ink can adopt independently and neither blocks the firmware landing.

Only A blocks `docs/companion-todo-list-design.md` phase A.
