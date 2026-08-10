# ToDo List Mode — Design Sketch

**STATUS (2026-08-10): §10 phase A is implemented in firmware, protocol doc and host harness, and is
now hardware-verified.** Landed: the `kFieldListDoc` (`0x08`) wire codec and its
host-buildable, allocation-free parser (`src/CompanionTodoDocument.{h,cpp}`), per-peer storage
(`src/CompanionPeerStore.{h,cpp}`'s `lists.bin`, the verbatim wire bytes — see §3 for the JSON
detour this superseded and why), `Screen::List` rendering and local navigation
(`src/CompanionTodoNav.{h,cpp}`, `src/activities/companion/CompanionModeActivity.cpp`, which now
holds the document in RAM for the screen's lifetime rather than re-reading SD per keypress — see
§8), and the offline icon-grid/picker entry point. Host-tested (`test/companion_todo_document/`,
`test/companion_todo_nav/`) and covered by an e2e `[list]` harness group
(`scripts/companion_e2e_test.py`) that ran against a real X3 reader on 2026-08-10 and passed
**17/17** — see `docs/companion-display-protocol.md`'s status warning for the exact assertions
covered and the two unrelated flaky screen-state checks (not in this group) seen in an earlier run
of the same session.

**STATUS (2026-08-10, later the same day): Phase B's offline check-off and sync-back are
implemented and hardware-verified.** Landed: the on-device diff
(`src/CompanionTodoDiff.{h,cpp}`, host-tested in `test/companion_todo_diff/`), its per-peer storage
(`list_state.bin` — **binary, not the JSON this document originally specified; see §3**), `Confirm`
toggling on `Screen::List`, and the `LIST_STATE_AVAIL`/`LIST_STATE_GET`/`LIST_STATE` conversation
on the Session characteristic. The full `scripts/companion_e2e_test.py` suite passed **159 passed,
0 failed, 0 skipped** against a real X3 reader on three consecutive runs. **Not** landed: phase C
(CompanionKit surface).

**Both phases are part of protocol v12, not a new version.** v12 already reserved
`ContentShape::List = 0x03` (`docs/companion-declared-shape-design.md`); phase A gave the
reservation a content field, and phase B's sync-back rides the Session characteristic v12 already
defines, adding neither a content field nor a message-layout change.

**Three things this sketch got wrong, corrected by the implementation (see §3 and §4):**

- **No `kUiCapabilityTodoList` capability bit exists**, despite §4 introducing one for gating
  `LIST_STATE`. The shape byte alone (`ContentShape::List`) is what the implemented device checks
  and what the offline picker filters on — §4's own earlier paragraph had already talked itself out
  of a capability bit for shape declaration, then reached for one again a few lines later for
  `LIST_STATE`. The implemented gate turned out to need no client declaration at all: the device
  announces its diff when it *has* one, and only a `LIST` peer structurally can.
- **The single-notification `LIST_STATE` this document specified cannot exist.** It does not fit
  the Session characteristic, at any realistic list size. §4 below now carries the replacement
  (an availability notify plus a phone-driven paginated pull) and the measurement of why the
  original was impossible, rather than quietly swapping one design for another.
- **`list_state.json` is `list_state.bin`.** §3's stated reason for JSON was falsified by this
  very slice; §3 records that rather than overwriting it.

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
  dodge a recursive parser, which isn't a real cost here — `companiontodo::parseDocument()`
  (§3/§4) walks nesting with a single forward pass and no allocation, so a hierarchical wire format
  costs nothing extra over the flat one it replaced. Nesting also settles §9's original open
  question about group order for free: order is array position, not a separate field or "first item
  wins" convention.
- **Group** = `{ groupId, label, items[] }`. A list's "ungrouped" items are just its first group with
  an empty label — no separate bucket type, one less case to render.
- **Item** = `{ itemId, text, checked }`.
- Ids (`listId`, `groupId`, `itemId`) are **device-opaque**, assigned and owned by the phone, same
  rule as `appId`/`installId`/content-id everywhere else in this protocol: the firmware compares them
  for equality and echoes them back, never generates or interprets them.

## 3. Storage layout

**This section was rewritten after the JSON choice below was implemented, measured, and reversed.
The original reasoning is kept (struck through in spirit, not in markdown) rather than quietly
overwritten, because the reversal — and the measurement behind it — is the whole point of this
revision.**

**Original choice (superseded):** follow **`peers.json`/`images.json`'s** idiom (`ArduinoJson` +
`PersistableStoreBase`), not `buttons.bin`/`icon.bin`'s verbatim-asset one — the todo document has no
byte-identity constraint the way an asset digest does, `Screen::List` has to parse it to render
regardless, so storing the already-parsed structure would cost nothing extra and keep the SD card
human-readable. The premise was **reusing machinery already in the firmware** (`ArduinoJson`), not
writing a new codec.

**What actually happened at implementation time:** measured against `ArduinoJson` v7's real allocator
(a custom `Allocator` tracking live bytes, since v7's `JsonDocument::memoryUsage()` always reports 0
now), a JsonDocument shaped the way this design assumed — a few hundred short items — cost ~49 KB
live, already uncomfortable next to the reassembly buffer resident alongside it. That discomfort has
since been measured directly rather than argued: the 2026-08-10 hardware run of the current
(verbatim-bytes) build reported `Free: 52112 bytes, Total: 234676, MaxAlloc: 51188` on-device — the
abandoned ~49 KB JsonDocument approach would have been perilously close to that `MaxAlloc` ceiling on
this exact part, before even accounting for the pathological item counts below. Worse, `kMaxListDocLen`
(16 KB) bounds transfer *bytes*, not item *count*: the wire format's per-item floor is 4 bytes (a
2-byte id, 1 checked byte, 1 zero-length `textLen`), so a legally-sized push can carry over 4000
near-empty items — built and measured that exact ~4092-item document at ~450 KB live in the same
JsonDocument approach, multiples of this device's entire ~380 KB RAM, from a push that broke no rule
this protocol enforced. A malformed-content DoS, not a hardware bug. The write side was rewritten as a
fully hand-rolled, host-tested JSON serializer (streaming to SD as the wire buffer was parsed, never
building a JsonDocument) to dodge exactly that cost — **which voided the original premise**: nothing
was being reused from `peers.json`/`images.json` anymore, `ArduinoJson` was still on the *read* path
only, and an artificial `kMaxListItems` = 512 item cap had to be invented purely to keep that
remaining reader's read-back bounded. Bespoke writer, RAM-hungry reader, and a cap that existed only
to make that reader safe — all in service of a "reuse JSON machinery" goal that no longer held.

**Current choice: store the verbatim wire bytes, exactly like `buttons.bin`/`icon.bin`.** The document
is validated with `companiontodo::parseDocument()` — the same allocation-free parser
(`src/CompanionTodoDocument.{h,cpp}`) that already had to exist to walk the wire push in the first
place — and, only once that returns `Ok`, written through unmodified. Reading it back for
`Screen::List` uses the identical parser. This is what a byte-identity asset gets for free and a
"the device parses it anyway" document does not automatically forfeit: since nothing downstream can
safely assume an item-count bound smaller than what `kMaxListDocLen` already implies, and the
allocation-free parser doesn't care how a document spends its 16 KB either way, there is no reader left
to protect — `kMaxListItems` is gone, and `lists.bin`'s bytes are the wire bytes, full stop.

```
peers/<peerKey>/
  lists.bin          the exact wire bytes of the phone's last full kFieldListDoc push, unmodified
  list_state.bin     u8 formatVersion=1, u32 revision, u16 count, count x { u16 itemId, u8 checked },
                     ascending by itemId — the deviations from lists.bin the user has made on the
                     device since that revision was pushed
```

**The local diff was specified here as `list_state.json`, and shipped as `list_state.bin`.** The
stated justification for JSON was: *"unlike `lists.bin`, it has no wire format to be verbatim of —
it is device-local, source-of-truth data that has no equivalent on the wire until Phase B pushes it
back to the phone."* Phase B is the slice that pushes it back, and it **gave the diff exactly one
wire format** (`LIST_STATE`'s `{ itemId:2, checked:1 }` entries, §4), which falsifies the premise
outright rather than merely outweighing it. Kept visible for the same reason the `lists.json`
reversal above is: the argument was sound when written and was killed by a later slice, which is
worth more on the record than a clean-looking document.

What the byte format buys, beyond not being wrong: the on-disk entries are **byte-identical to the
wire body**, so serving a pull is one header read and one seek to `7 + 3*offset` — no materialised
`Diff`, no re-encoding step to keep in sync, and nothing resident. A JSON file would have had to be
parsed into a structure (1096 bytes, §8) on every request, to answer a question the file's own
layout can answer by arithmetic. Ascending-by-`itemId` order is what makes that offset addressing
stable across independent pulls, so it is a format rule, not an implementation convenience.

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
    heap-discipline convention) for the duration of one push, validate and write straight through to
    `lists.bin` (§3), free immediately after. Alloc-per-push is normally something to avoid here for fragmentation
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

### Sync-back: availability notify + phone-driven pull

**This replaces the single `LIST_STATE` notify this section originally specified**, which was:
one notification on `HELLO_OK`/`FOREGROUND` for a `kUiCapabilityTodoList` peer, carrying
`{ revision, count, count × { itemId, checked } }` — the whole diff at once. **It is physically
impossible on this characteristic**, and that is worth stating rather than skipping:

- The Session characteristic's documented floor is **103 bytes** (`docs/companion-display-protocol.md`,
  "Session characteristic": `HELLO` is up to 103 bytes and "a central that cannot get past 23 cannot
  use v8 at all"), and it carries the invariant that **every message is a single write or a single
  notification — this characteristic is never chunked**. A realistic shopping-list diff is 150–200
  entries × 3 bytes. It does not fit, and it is not close.
- Bursting a run of notifies instead trades a size problem for a buffer one: NimBLE's msys mbuf pool
  is finite, and this firmware already paces `kSessImageChunkAck` at one per 32 chunks for exactly
  that reason (`src/CompanionBle.cpp`), with `notifySession()`'s `bool` return discarded — so a
  dropped notify in the middle of a burst would be silent, and the phone would have no way to know
  which window it lost.

**What ships instead.** The device announces availability; the phone pulls windows:

```
device -> phone  LIST_STATE_AVAIL  0x8B  sessionId:1  revision:4  count:2          =  8 bytes
phone  -> device LIST_STATE_GET    0x05  sessionId:1  offset:2                     =  4 bytes
device -> phone  LIST_STATE        0x8C  sessionId:1  revision:4  offset:2
                                         total:2  n:1  n x { itemId:2, checked:1 } = 11 + 3n bytes
```

`kListStateEntriesPerNotify = 30`, so `LIST_STATE` maxes out at **101 bytes** — inside the
already-documented 103-byte floor, which is why this needs no MTU renegotiation and no runtime MTU
query. **The 30 is fixed, deliberately not derived from the negotiated MTU**, on the same reasoning
the protocol doc already applies to `kMaxContentIdLen` ("treat 32 bytes as the contract"): a
constant is host-testable and identical for every client, where a per-connection page size would
make the pagination boundary — the one thing a pull's correctness turns on — a property of whichever
central connected.

**The never-chunked invariant survives, and that is the point of this shape, not a side effect.**
Every message here is complete and self-describing: each `LIST_STATE` carries its own `revision`,
`offset` and `total` and means something on its own. A sequence of independent whole messages is a
different thing from chunking one message — nothing is a fragment, there is no reassembly state in
either direction, and a lost message costs its window rather than desynchronising a stream. The
protocol doc's sentence is reaffirmed there, not relaxed.

**Semantics:**

- **The device is stateless across a pull.** Each `GET` is an independent seek into `list_state.bin`;
  no cursor is held. Pulls may be repeated, reordered or abandoned freely, which is what makes a
  dropped notification cheap.
- **`n = 0` is the terminator, not an error.** A `GET` with `offset >= total`, for an unknown
  `sessionId`, or for a peer with no stored diff is answered `n = 0` (`revision`/`total` as known,
  `0` when unknown). The device logs nothing.
- **The gate is "has a non-empty diff", full stop** — no capability bit, and specifically not the
  `kUiCapabilityTodoList` this document twice reached for. A non-`LIST` peer structurally cannot
  have a diff (no `lists.bin` means no reachable on-device toggle), so the diff's existence *is* the
  declaration. A `LIST` peer that never pushed a document hears nothing, which beats an empty
  message the phone has to interpret.
- **`LIST_STATE_AVAIL` is sent at three points:** immediately after `HELLO_OK` (`admitPeer()`),
  alongside `FOREGROUND` (`setForeground()`), and live from the on-device toggle when the user
  checks something off while the peer is connected.
- **The `HELLO_OK` ordering is load-bearing.** `storeListDocument()` clears the diff unconditionally
  on every new document, and a push requires `ACQUIRE` → foreground, which cannot precede
  `HELLO_OK`. So the phone always learns about a pending diff *before* it is capable of destroying
  one. A phone that pushes first anyway is discarding edits it was told about, which is its call to
  make; the device does not arbitrate.
- **Entries are deviations, not absolute states**, and `revision` is recorded, never interpreted —
  the asset-digest pattern (§5 of the multi-app design: "device reports opaque state, app decides
  what's stale, app pushes") applied to list state instead of an asset tag. The phone merges and
  re-pushes a new `kFieldListDoc` at a new revision; the device stores it wholesale and clears the
  diff.

**No new content field.** `0x09` stays free: this is a conversation the device initiates and the
phone answers piecewise, carrying nothing that owns the screen — none of which `START`/`CHUNK`/`END`
framing models.

**No version change.** Nothing about an existing message's layout changed, and the three opcodes
live on the Session characteristic v12 already defines. The capability characteristic advertises
the sync-back as byte 5 bit 5, alongside bit 4's declared content shape (byte 5 reads `0x3F`), with
the 23-byte layout unchanged.

## 5. On-device UX

- `Screen::List` added to `src/activities/companion/CompanionModeActivity.h:26-35`.
- Navigation, entirely local, no protocol surface — same "screen-local, no BLE notification" class as
  the existing gallery Up/Down paging (`src/activities/companion/CompanionModeActivity.cpp:792`
  onward) and local text pagination:
  - Up/Down: move item cursor, paging the visible window when it runs off-screen.
  - Left/Right: switch between lists within the document.
  - Confirm: toggle checked on the item under cursor.
  - Back: leave `Screen::List`, back to the icon grid.
- A toggle writes through to `list_state.bin` immediately (small, infrequent writes — nothing like
  the per-CHUNK write rate of an image push), via the same temp-file-then-rename discipline
  `lists.bin` uses, so a mid-write failure cannot clobber previously-good edits. It records a
  **deviation** from the document's own `checked` value: toggling an item back to what the document
  says removes its entry rather than recording a redundant one, so a user fidgeting with one
  checkbox cannot grow the table and "empty diff" means exactly "nothing to sync" with no comparison
  pass. `lists.bin` itself is never edited — see `src/CompanionTodoDiff.h` for why the pushed
  document has to stay the phone's, verbatim.
- A toggle that needs a new entry when the table is full (`kMaxDiffEntries` = 512) is **refused and
  reported**, not silently dropped and not silently evicting another entry: the user is told the
  edit did not take. The cap is sized so a legal 16 KB document's every item can carry a deviation.
- If the peer is connected, a toggle also sends `LIST_STATE_AVAIL` (§4) — the phone learns
  immediately rather than at next connect.
- Rendering redraws just the toggled row's checkbox glyph via the windowed-refresh path measured in
  §9 (~430 ms against ~2979 ms for a full refresh), so no coalescing is needed — a rendering detail,
  not a protocol one.
- What `Screen::List` draws is the document's `checked` **overridden by the diff**; the two agree by
  construction immediately after a push, since storing a document clears the diff.
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
- The diff is likewise one flat `itemId -> checked` map across every list in the document,
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
- `CompanionClient` gains an event for `LIST_STATE_AVAIL` (mirrors how asset digests already
  surface), a pull that loops `LIST_STATE_GET` until `n = 0` and hands back one assembled
  `[itemId: Bool]` of deviations, and a `pushTodoDocument(_:)` call. The pagination belongs here,
  not in app code: every consumer app would otherwise reimplement the same loop and the same
  terminator rule. The merge logic — combining the device's reported diff
  with the app's own edits into a new revision — lives here, in the app-facing package, per "the
  device never merges" in §4/§6.

## 8. Memory

| Item | Where it lives | Cost |
|---|---|---|
| Document (`lists.bin`) | SD | 0 RAM at rest |
| Local diff (`list_state.bin`) | SD | 0 RAM |
| `companiontodo::Diff` while a toggle is applied | heap, materialised for one edit and dropped | 1096 bytes, transient — **0 permanently resident**. Bit-packed `checked` rather than a `struct{u16,u8}[512]` the compiler would pad to 2048. Serving a pull materialises nothing at all: a header read plus a seek (§3) |
| In-flight document reassembly (a push arriving) | heap, `makeUniqueNoThrow`, freed right after validating and writing to SD | ≤ `kMaxListDocLen` = 16 KB, transient — **not** a fixed global buffer (see §4's note on why this deliberately does not copy the title/body pattern) |
| Document buffer while `Screen::List` is up | heap, `makeUniqueNoThrow`, held by `CompanionModeActivity`, sized to the stored document, freed the moment the screen is left | ≤ `kMaxListDocLen` = 16 KB, held for the screen's lifetime rather than re-read from SD per keypress — see §3's "current choice" and `docs/companion-display-protocol.md`'s "List document field" |
| List/cursor nav state | RAM, foreground peer only | comparable to the ~200 B gallery nav state (§11 of the multi-app design) |

Net new *permanently resident* RAM is well under 1 KB, matching the multi-app design's own budget
outcome. Two of the rows above are real but transient 16 KB allocations, never both live at once
(a push's reassembly buffer is long freed before `Screen::List` is ever entered over its result): one
alive only for the duration of one push, the other alive only while `Screen::List` is on screen —
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
- **B — Offline checking + sync-back.** §5–§6. The genuinely new mechanism, and still v12: three
  Session-characteristic opcodes, no content field, no message-layout change.
  **The test path in this bullet was superseded.** It proposed simulating a local toggle by writing
  `list_state.json` directly in a test build, which would have proven the storage and wire halves
  while stepping around the only part that is new: the button seam. The harness instead injects
  `CMD:CBTN` through the **actual** button path, so a `Confirm` that never reaches `Diff::applyToggle()`
  fails the test rather than being bypassed by it. Round trip: push document → `CMD:CBTN` a toggle →
  assert `LIST_STATE_AVAIL` → pull with `LIST_STATE_GET` → assert the entries, the `n = 0`
  terminator, and that a re-push clears the diff. Per this repo's "every fix starts red" /
  no-phone-as-test-harness rules — this is protocol and firmware behaviour, not something that needs
  a phone to prove.
- **C — CompanionKit surface.** §7. Can start once A's wire format is stable; does not need B to land
  first, since the merge logic it owns is exercised by the harness fake in B, not required to be a
  real iOS app for either A or B to be provable.
