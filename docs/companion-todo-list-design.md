# ToDo List Mode — Design Sketch

**STATUS (2026-08-10): §10 phase A is implemented in firmware, protocol doc and host harness;
nothing has run on hardware.** Landed: the `kFieldListDoc` (`0x08`) wire codec and its
host-buildable parser (`src/CompanionTodoDocument.{h,cpp}`), per-peer storage
(`src/CompanionPeerStore.{h,cpp}`'s `lists.json`, streamed write, ingest-time item cap),
`Screen::List` rendering and local navigation (`src/CompanionTodoNav.{h,cpp}`,
`src/activities/companion/CompanionModeActivity.cpp`), and the offline icon-grid/picker entry
point. Host-tested (`test/companion_todo_document/`, `test/companion_todo_nav/`,
`test/companion_list_json_writer/`) and covered by an e2e `[list]` harness group
(`scripts/companion_e2e_test.py`) that is written but **not run against a device** — see
`docs/companion-display-protocol.md`'s status warning, same honesty this doc's own prior status
line asked for. **Not** landed: phases B (offline checking, `list_state.json`, `LIST_STATE`
sync-back) and C (CompanionKit surface) below — Phase A is read-only, full stop.

**This shipped as part of protocol v12, not a new version.** v12 already reserved
`ContentShape::List = 0x03` (`docs/companion-declared-shape-design.md`); this work gave that
reservation a content field rather than bumping anything.

**Two things this sketch got wrong, corrected by the implementation (see §4):**

- **No `kUiCapabilityTodoList` capability bit exists**, despite §4 introducing one for gating
  `LIST_STATE`. The shape byte alone (`ContentShape::List`) is what the implemented device checks
  and what the offline picker filters on — §4's own earlier paragraph had already talked itself out
  of a capability bit for shape declaration, then reached for one again a few lines later for
  `LIST_STATE`. Since `LIST_STATE` itself is unimplemented (next point), so is the bit that would
  have gated it.
- **No `LIST_STATE` notify exists.** It is Phase B, entirely unimplemented — no offline check-off,
  no `list_state.json`, no sync-back of any kind. §3's `list_state.json` bullet and §4's `LIST_STATE`
  paragraph both describe this unimplemented mechanism, not current behaviour.

**Stale line citations, not fixed here:** `src/CompanionBle.h:209` (§4) is now the `kFieldTodoList`
family around line 92 (`kMaxListDocLen`) and 283 (`kFieldListDoc`) — the file grew underneath the
citation. `src/activities/companion/CompanionModeActivity.h:26-35` (§1, §5) is now roughly lines
44-54 for the `Screen` enum. Treat every line number in this document as approximate; the code is
the truth, not the citation.

Nothing here is wire-final beyond what §10 phase A actually shipped. Written to be picked up as a
phase A implementation plan once reviewed, in the style of `docs/companion-multi-app-design.md`,
which this builds directly on top of rather than beside.

Primary use case: shopping lists. Sync from the phone, then walk the store with just the reader,
checking items off with no phone in hand, and have the check-offs land back on the phone next time
it's nearby.

---

## 1. What this is, precisely

Not a new top-level firmware mode. `main.cpp` boots straight into `CompanionModeActivity`
(root `CLAUDE.md`), and v6+ already generalized that activity to render whatever shape of content
the foreground peer declares — title/body, or a photo. ToDo List is a third shape: a new
`Screen::List` alongside `Screen::Text` / `Screen::Image`
(`src/activities/companion/CompanionModeActivity.h:26-35`), reached by a peer that declares a new UI
capability bit, using the session/peer/asset-digest machinery §3–§7 of the multi-app design already
built — not a parallel mechanism.

The one real departure from that design's principle: today the device never mutates app data, only
forwards raw button events or does closed, on-device-only screen actions (`LocalPagePrev`,
`LocalSleep`). Checking an item off *is* a data mutation, and it has to survive with no phone
present, or "walk around the store with just the reader" doesn't work. §5 below is why that's an
acceptable, bounded exception rather than a crack in "dumb firmware, smart phone": the device still
never merges or decides anything, it only accumulates a diff and hands it back verbatim.

## 2. Data model

- **Document** = every list for one peer, pushed and replaced as a single unit (see §4 — user
  preference: sync all lists at once, not per-list, given how small this data is).
- **List** = `{ listId, title, groups[] }` — **hierarchical**, not a flat item array with a group
  label reference. Revised from the first draft's flat-plus-reference shape: that shape existed to
  dodge a recursive parser, which isn't a real cost here (see §3 — storage moves to `ArduinoJson`,
  the same library `peers.json`/`images.json` already use, and nested JSON costs nothing extra
  there). Nesting also settles §9's original open question about group order for free: order is
  array position, not a separate field or "first item wins" convention.
- **Group** = `{ groupId, label, items[] }`. A list's "ungrouped" items are just its first group with
  an empty label — no separate bucket type, one less case to render.
- **Item** = `{ itemId, text, checked }`.
- Ids (`listId`, `groupId`, `itemId`) are **device-opaque**, assigned and owned by the phone, same
  rule as `appId`/`installId`/content-id everywhere else in this protocol: the firmware compares them
  for equality and echoes them back, never generates or interprets them.

## 3. Storage layout

Extends `CompanionPeerStore`'s existing per-peer directory (`src/CompanionPeerStore.h:19-27`), but
following **`peers.json`/`images.json`'s** idiom (`ArduinoJson` + `PersistableStoreBase`,
`src/CompanionPeerStore.cpp:5-81`), not `buttons.bin`/`icon.bin`'s verbatim-asset one. Those two are
stored as exact pushed bytes because the asset-digest mechanism needs them back byte-identical and
never parses them; the todo document has no such constraint — `Screen::List` has to parse it to
render regardless, so storing the already-parsed structure costs nothing extra and keeps the SD card
human-readable, which is worth it here since size isn't a real constraint (a shopping list is a few
KB, nothing like a raw framebuffer push):

```
peers/<peerKey>/
  lists.json        { revision, lists: [ { listId, title, groups: [ { groupId, label, items: [
                     { itemId, text, checked } ] } ] } ] } — the device's parsed copy of the phone's
                     last full push
  list_state.json    { revision it was taken against, checked: { itemId: bool } } — items the device
                     has toggled locally since lists.json's own revision was pushed
```

The wire push itself (§4) stays binary framing like every other field — `Screen::List` decodes it
once on arrival and writes the JSON straight through, the same shape as how `images.json`'s index is
synthesized by firmware from binary pushes rather than pushed as JSON itself. `list_state.json` is
the only genuinely new *kind* of file this feature needs: everywhere else, SD storage is a cache of
something the phone already knows; this one is source-of-truth data that hasn't reached the phone
yet.

## 4. Wire format

- **New field `kFieldListDoc = 0x08`** (next free per `src/CompanionBle.h:209`). Pushed as a whole
  document — no incremental add/remove-item ops, matching how `kFieldUiDeclaration` and the icon are
  always full replaces. Reassembled in RAM like title/body (`kFieldTitle`/`kFieldBody`), not streamed
  straight to SD like `kFieldImage`: list text is small.
  - **Cap: `kMaxListDocLen = 16 KB`**, chosen against the actual remaining headroom, not picked round:
    current steady-state DRAM usage is 154,857 / 321,296 bytes (48.2%), leaving ~166 KB
    (`pio run -e default`'s size report, measured while doing this design pass) — even the full 16 KB
    resident would be a rounding error against that.
  - **But it must NOT follow title/body's storage pattern.** `kFieldTitle`/`kFieldBody` reassemble
    into `CompanionBatchModel::titleBuf_`/`bodyBuf_`, fixed `kMaxFieldLen` (4 KB) arrays inside the
    **global** `g_batchModel` instance (`src/CompanionBatchModel.h:109,113`,
    `src/activities/companion/CompanionModeActivity.cpp:102`) — resident for the firmware's entire
    life, not just during a push, which is fine at 2×4 KB on a hot path exercised by every content
    push. A list document is 4x that size and pushed rarely (a full document replace, not a per-item
    op) — following the same fixed-global-buffer pattern would make 16 KB permanently resident for a
    feature most sessions never touch. Instead: heap-allocate via `makeUniqueNoThrow` (per this repo's
    heap-discipline convention) for the duration of one push, write straight through to `lists.json`,
    free immediately after. Alloc-per-push is normally something to avoid here for fragmentation
    reasons, but a todo-list push is infrequent enough (not a hot per-CHUNK or per-frame path) that
    the tradeoff favors not carrying 16 KB permanently over avoiding one alloc/free per full-document
    sync.
- **Document carries a `revision : u32`**, monotonically increasing, assigned by the phone on every
  push. This is what makes "sync all lists together" (§6) coherent: one number describes the whole
  document's freshness, not one per list.
- **A declared content shape of `LIST`**, per `docs/companion-declared-shape-design.md` — *not* a new
  capability bit alongside `kUiCapabilityImageGallery`, which was this doc's first proposal. That
  proposal was wrong in an instructive way: it assumed content shape was already declared at
  handshake, when in fact the implemented protocol is purely reactive (last completed push owns the
  screen, `docs/companion-display-protocol.md:924-928`). A list cannot live under reactive rules —
  it holds a cursor and unsynced local check-offs that a stray title/body push from the same app
  would silently destroy, and it claims too many buttons to negotiate per screen. See §2 of the
  declared-shape doc. A peer declaring `LIST` also gets the picker entry point the capability bit was
  reaching for: CONFIRM on its sleep-screen icon enters `Screen::List` over that peer's stored
  document — the same "local SD browse of content the app already pushed" argument that makes the
  gallery picker not-a-launcher (§8 of the multi-app design). **This is also why the shape is
  declared in the persisted UI declaration rather than on `ACQUIRE`:** that entry point runs with no
  phone connected, so the device must know a peer's shape while disconnected.
- **New Session-characteristic notify, `LIST_STATE`**, sent once on `HELLO_OK`/`FOREGROUND` for a
  `kUiCapabilityTodoList` peer, before the peer pushes anything: `{ revision, count, count ×
  { itemId, checked } }` — the device's current `list_state.json` diff against whatever revision it
  has. This is the asset-digest pattern (§5 of the multi-app design: "device reports opaque state,
  app decides what's stale, app pushes") applied to list state instead of an asset tag. The device
  never decides whether its diff is stale or how to merge it; the phone does, then re-pushes a new
  `kFieldListDoc` at a new revision, which the device stores wholesale and against which it clears
  `list_state.json`.

All additive — no framing change to an existing op, no length change to an existing notification —
so, following the pattern already used for `ASSET_ACK`/`IMAGE_STATUS`, this should not need the kind
of breaking cutover v6 was. Still needs a version bump to advertise the new capability bit and field
in the capability characteristic, but old clients that never set `kUiCapabilityTodoList` see none of
this.

## 5. On-device UX

- `Screen::List` added to `src/activities/companion/CompanionModeActivity.h:26-35`.
- Navigation, entirely local, no protocol surface — same "screen-local, no BLE notification" class as
  the existing gallery Up/Down paging (`src/activities/companion/CompanionModeActivity.cpp:792`
  onward) and local text pagination:
  - Up/Down: move item cursor, paging the visible window when it runs off-screen.
  - Left/Right: switch between lists within the document.
  - Confirm: toggle checked on the item under cursor.
  - Back: leave `Screen::List`, back to the icon grid.
- A toggle writes through to `list_state.json` immediately (small, infrequent writes — nothing like
  the per-CHUNK write rate of an image push). Rendering redraws just the toggled row's checkbox glyph
  where the panel's partial-refresh path allows it, full list redraw otherwise — a rendering detail,
  not a protocol one.
- This is the one place a button's meaning isn't declared by the peer's `ButtonRouting` map (§7 of
  the multi-app design) — it's implicit in being on `Screen::List`, the same way gallery Up/Down and
  text pagination are already implicit in their screens rather than routed. No new `ButtonRouting`
  enum value needed.
- **What makes that safe is the declared shape**, not the screen state. The gallery gets away with
  claiming Up/Down only because it takes them when the peer's own map left them unclaimed
  (`docs/companion-multi-app-design.md:96-106`); a list needs Up/Down/Left/Right/Confirm, which is
  too much of the device to negotiate that way. Because a `LIST` peer declared itself as one before
  it ever reached the screen, its button map and the list's own navigation are known not to conflict
  up front, rather than being reconciled per redraw.

## 6. Sync model

User-confirmed: sync the whole document together, not list-by-list. This simplifies what was an open
question into: **one revision number for the entire document**, not one per list. Consequences:

- The phone always pushes every list in one `kFieldListDoc`, even to change one item on one list.
  Fine at this size — the whole point of confirming this now is that a few KB of shopping-list text
  makes per-list revisioning not worth its bookkeeping.
- `LIST_STATE`'s diff is likewise one flat `itemId -> checked` map across every list in the document,
  since `itemId` is already globally opaque and unique within a peer's document, not scoped to a
  list.
- Failure mode this accepts: two lists edited offline in the same session and reconciled together is
  fine (last-write-wins per item, phone's call); two *different devices* offline-editing the same
  peer's lists at once is out of scope — same single-peer, single-device assumption the rest of this
  protocol already makes (one BLE link, one foreground session).

## 7. CompanionKit surface (first-class, per direction)

Sibling repo, same versioning discipline as everything else there (package major = protocol
version). New first-class Swift types, not opaque-bytes round-tripping:

- `TodoItem { itemId, text, checked }`, `TodoGroup { groupId, label, items: [TodoItem] }`,
  `TodoList { listId, title, groups: [TodoGroup] }` — mirrors §2's nesting, not a flat-plus-reference
  shape.
- `TodoDocument { revision, lists: [TodoList] }` — the `kFieldListDoc` codec, alongside
  `UiDeclaration.swift`/`ContentFramer.swift`'s existing pattern.
- `CompanionClient` gains an event for the `LIST_STATE` notify (mirrors how asset digests already
  surface) and a `pushTodoDocument(_:)` call. The merge logic — combining the device's reported diff
  with the app's own edits into a new revision — lives here, in the app-facing package, per "the
  device never merges" in §4/§6.

## 8. Memory

| Item | Where it lives | Cost |
|---|---|---|
| Document (`lists.json`) | SD | 0 RAM |
| Local diff (`list_state.json`) | SD | 0 RAM |
| In-flight document reassembly | heap, `makeUniqueNoThrow`, freed right after the write to SD | ≤ `kMaxListDocLen` = 16 KB, transient — **not** a fixed global buffer (see §4's note on why this deliberately does not copy the title/body pattern) |
| List/cursor nav state | RAM, foreground peer only | comparable to the ~200 B gallery nav state (§11 of the multi-app design) |

Net new *permanently resident* RAM is well under 1 KB, matching the multi-app design's own budget
outcome — the 16 KB reassembly buffer is real but transient, alive only for the duration of one push,
against ~166 KB of measured headroom (see §4).

## 9. Open questions — resolved

~~Group ordering~~ — resolved by §2's move to hierarchical nesting: order is array position, no
separate field needed.

~~Reassembly cap size~~ — **resolved: 16 KB, heap-transient, not a global buffer.** Settled by reading
the actual measured RAM budget (`pio run -e default`'s size report: 154,857 / 321,296 bytes DRAM used,
~166 KB headroom) rather than picking a round number — see §4's full reasoning and its correction to
this doc's earlier, wrong assumption that the title/body reassembly buffer is already transient (it
isn't; it's a fixed 4 KB × 2 global array, `src/CompanionBatchModel.h:109,113`). This is a resolvable
by-reading-the-code question, not one that needed a device — no on-device test was run for this one.

~~Partial-refresh on toggle~~ — **resolved on hardware: viable, no coalescing needed.** `HalDisplay`
had no windowed-refresh entry point (`GfxRenderer::displayWindow` was a commented-out declaration at
`lib/GfxRenderer/GfxRenderer.h:194`, one layer above a working but unwired SDK implementation,
`FreeInkDisplay::displayWindow`, `freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:672`)
— wired it through (`HalDisplay::displayWindow`, `GfxRenderer::displayWindow`, reusing the alignment
helper `readFramebufferRegion`/`writeFramebufferRegion` already share) and measured on an X3 with a
throwaway serial-triggered probe: a full `FAST_REFRESH` took **2979 ms**; a windowed refresh of a
single ~40px list-row took **430 ms** — about 7x faster, and comfortably under the ~2.2s multi-pass
grayscale settle already accepted as normal for a full content push (`src/CompanionBle.h:224-227`).
**A toggle can refresh its own row without coalescing.** The `displayWindow` plumbing is now real,
committed infrastructure for phase B, not a documentation artifact — the throwaway probe command
itself was removed after measuring.

## 10. Sequencing

**Blocked on `docs/companion-declared-shape-design.md` phase A** (the shape declaration and its
enforcement). That is a prerequisite, not a parallel track: without a declared shape there is no safe
way for `Screen::List` to hold a cursor or claim its buttons — see §4 above and §2 of that document.

- **A — Wire + storage + `Screen::List` rendering + read-only sync.** §2–§4, §8. Gets a pushed
  document on screen, paginated, no offline checking yet. Provable with the existing host-harness
  discipline (`scripts/companion_e2e_test.py`) since it's push-and-render, same shape as text/image
  pushes today.
- **B — Offline checking + `LIST_STATE` sync-back.** §5–§6. The genuinely new mechanism. Needs its
  own host-harness coverage for the round trip (push document → simulate local toggle by writing
  `list_state.json` directly in a test build → reconnect → assert `LIST_STATE` reports it), per this
  repo's "every fix starts red" / no-phone-as-test-harness rules — this is protocol and firmware
  timing behavior, not something that needs a phone to prove.
- **C — CompanionKit surface.** §7. Can start once A's wire format is stable; does not need B to land
  first, since the merge logic it owns is exercised by the harness fake in B, not required to be a
  real iOS app for either A or B to be provable.
