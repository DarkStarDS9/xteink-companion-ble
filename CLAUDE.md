# xteink-companion-ble — Agent Guide

**Read [.skills/SKILL.md](.skills/SKILL.md) first.** It is the shared engineering guide inherited
from upstream CrossPoint Reader — hardware constraints, the HAL, memory rules, ESP32-C3 pitfalls,
build system, cache formats. All of it still applies; it is deliberately left unmodified so
upstream's improvements keep merging cleanly.

This file covers only what is **different about this fork**.

---

## What this repo is

A BLE companion-display firmware, not an e-reader. The device boots straight into
`CompanionModeActivity` ([src/main.cpp:423](src/main.cpp)) and is driven by a paired phone app.

**It serves several consumer apps, not one** — see [ROADMAP.md](ROADMAP.md). This is the single most
important framing for scoping decisions: a feature that only one app would use generally belongs in
that app's repo, not here.

Scope test: [SCOPE.md](SCOPE.md). **Do not apply upstream's scope reasoning** — upstream bans
"interactive apps" and "news aggregators", which are exactly this fork's purpose.

## Guiding principle: dumb firmware, smart phone

Where behaviour could live on either side, it goes on the phone. The device records raw events
(button id, hold duration) and renders what it is told; it does not interpret. Protocol v5
deliberately replaced semantic button actions with raw button identity for this reason.

## Memory reality

NimBLE costs ~63 KB on top of the ~51 KB framebuffer (52,272 bytes; the panel measures 528x792, not
the 800x480 some inherited notes assume), on a 380 KB no-PSRAM part. The budget is
tighter here than upstream's guide assumes. Any proposal involving a new buffer must say where the
memory comes from — streaming to an SD scratch file is usually the answer.

## The protocol is the product

[docs/companion-display-protocol.md](docs/companion-display-protocol.md) is **authoritative** for
the wire format. Consumer apps depend on it.

- **Never bump the protocol version unless you are explicitly told to.** This overrides every
  instinct you have about versioning, and it overrode a v13 bump this file's earlier wording had
  invited (2026-08-10, ToDo List Phase B). We are the only consumers — CompanionKit, SpokenFeeds,
  Snap2Ink are all this project's own and release in lockstep — so there is no deployed client to
  strand and **backward compatibility is not a requirement**. Treat the current version as still in
  development: an additive change folds into v12's definition, and the docs are rewritten to describe
  v12 as always having included it. A capability feature bit inside an unchanged 23-byte layout is
  fine as a discovery mechanism and does not by itself justify a bump.
  - Everything the ToDo List functionality needed — the `LIST_STATE` sync-back opcodes and the
    byte-5 feature bit — is **v12**. Not v13.
  - Still update `docs/companion-display-protocol.md` in the same commit as any wire change. That
    part was never about the version number.
  - Under consideration: **resetting the version to v1** once the dust settles, before this is
    announced or presented. Do not do it unprompted, but do not write anything that would make it
    painful either.
- Field ids live in [src/CompanionBle.h](src/CompanionBle.h). Implemented: `0x01` title, `0x02` body,
  `0x03` content-id, `0x04` image (raw packed 2bpp, full screen, no header — see
  `RawBitmapToFramebufferConverter` and the protocol doc's "Image field" section; this replaced an
  earlier PNG-based format, dropped because PNGdec's ~44 KB working set never fit alongside NimBLE
  on this part), `0x05` UI declaration, `0x06` icon, `0x07` tag state, `0x08` list document (ToDo
  List, `LIST` shape only — see `src/CompanionTodoDocument.h` and
  `docs/companion-todo-list-design.md`). Next free: `0x09`.
- The wire is at **v12**. `0x08` is no longer read-only: the device toggles items locally and syncs
  the diff back over the Session characteristic (`LIST_STATE_AVAIL` / `LIST_STATE_GET` /
  `LIST_STATE`, opcodes `0x8B` / `0x05` / `0x8C`), which is why `0x09` is still free — a sync-back is
  a conversation, not a content push, so it needs no field id.
- The v6 shape — sessions, phone:app pairing, per-peer SD storage, button labels/routing — is in
  [docs/companion-multi-app-design.md](docs/companion-multi-app-design.md) and implemented in
  firmware, though unproven on real hardware (see the protocol doc's status warning). It was a
  **clean break**: v6 requires a handshake, so a v5 client gets no error, just silence.

## The phone is not a test harness

**Do not make the user pick up their phone to reproduce something.** The reader
(`/dev/cu.usbmodem*`) and the Sniffle sniffer (`/dev/cu.usbserial-*`) are both attached to the same
Mac the agent runs on. Anything that is *protocol*, *firmware*, or *app behaviour* — batch framing,
the final-field flag, push cadence, `RENDER_STATUS` correlation, timeout interactions, a client that
waits on an acknowledgement — is reproducible from a host script, and must be. Tying iterative
testing to a human tapping a phone is what produced the circular, bug-driven stretches in this
work's history.

- Extend the harness that already exists: `scripts/companion_protocol.py` (the shared client),
  `scripts/companion_e2e_test.py`, `scripts/push_companion_content.py`. Do not write a fourth
  one-off script.
- A harness that mimics a consumer app must mimic its **behaviour**, not just its bytes — SpokenFeeds
  pushes an article as a batch and then *waits* for the render acknowledgement before starting audio.
  A harness that fires and forgets cannot reproduce the failures that matter.
- **What a host harness genuinely cannot prove**: on 2026-08-07 DLE moved out of this list — the
  macOS central reproduced the `onConnect()` LLCP collision exactly (a DLE-carrying build died at
  39996 ms with HCI `0x22`; the same build without the request survived a 3-minute soak), so the
  DLE fix is host-A/B-provable and was proven that way. What remains genuinely phone-territory:
  the exact parameter GRANTS an **iOS** central issues (latency/interval numbers — macOS's central
  stack is not iOS's and is not steerable), and iOS-specific PHY negotiation behaviour. Those need
  the phone plus the sniffer. Everything else does not, and claiming otherwise is how a phone-only
  loop gets rationalised.
- Known obstacle: host BLE via `bleak` fails with `DENIED_BY_UNKNOWN` when run **inside tmux** — a
  macOS TCC quirk, not a code bug. Run the harness from a plain terminal, or drive it as a launched
  process outside the multiplexer. Serial over USB is unaffected.

## Every fix starts red

Every bug fix starts with a failing test at the lowest level that can express it, not the highest
one that happens to be handy. Prefer, in order:

1. **Host unit test** (`test/`, seconds, no hardware) — if the seam exists. `CompanionBatchModel` and
   `CompanionConnPolicy` were extracted out of hardware-only code on purpose so the batch state
   machine and the conn-param profile policy — the two areas that have regressed the most — live
   here now. If you are touching either, the seam already exists; use it.
2. **Harness assertion** (`scripts/companion_e2e_test.py` and friends) — when the bug is protocol or
   timing behaviour a host unit test cannot express, per "The phone is not a test harness" above.

A fix that lands without a new or updated test states its reason in the commit message — "no seam
yet" is not a reason, "this is a rendering/layout change with no host seam" is.

A policy change over an already-verified mechanism (conn-param profiles, batch timing) needs its own
measurement before landing, not just a plausible story — see the 2026-08-06 and 2026-08-07 git
history for what shipping on a plausible story alone has cost here twice already.

`scripts/run_companion_tests.sh` is the loop: run it, watch it fail red, fix, watch it go green.
Plain terminal for anything BLE (see the tmux note above), and the serial port must be free before
flashing — close any open monitor first.

## Upstream relationship

- **`companion`** is the trunk and default branch. **`develop`** is a pristine mirror of
  `upstream/develop` — never commit to it.
- Sync by **merging**, never rebasing: rebasing rewrites SHAs and strands submodule pins in consumer
  repos.
- **Generic fixes go upstream**, not into the fork — crashes, driver bugs, leaks, formatting. Use an
  `upstream-fixes/*` branch cut from `develop`. Carried patches are a recurring merge cost.
- The boot path in `src/main.cpp` is the one deliberate divergence and conflicts on most syncs.
  Keep the companion side.

## Delegate research and implementation to subagents

**If you are an Opus- or Fable-class model, hand research and implementation to subagents by
default.** Do not read a pile of source files, run a long web-search sweep, or write a multi-file
change directly in the main thread. Spawn agents for it and keep the main context for deciding,
reviewing their diffs, and talking to the user.

Why this is a rule and not a preference: the main thread is the only place that holds the thread of
the argument — what has been measured, what was disproved, what the user actually asked for. Fill it
with file contents and search results and that thread is what gets summarised away first. A session
that has read everything itself ends up **less** able to reason about the whole than one that
delegated and kept its own context for the reasoning.

- Give each agent a narrow question and the constraints, including what it must **not** touch.
- Require file:line citations, and confidence labels on research claims.
- **Review the diff yourself.** A subagent's summary is a claim, not evidence — agents in this repo
  have reported "done" on changes that were subtly wrong, and the review is the whole point of
  staying out of the weeds.
- Sonnet-class models running as the main thread may work directly; the tradeoff is different.

## Agent worktrees

Work done in a `.claude/worktrees/*` checkout lives on a throwaway `worktree-*` branch. It is not
done until it lands on `companion` in the **main repo checkout**
(`/Users/rainer/Git/xteink-companion-ble`, not the worktree path) — commit in the worktree, then
merge or cherry-pick onto `companion` there. Don't leave finished work stranded on a worktree
branch; don't push a `worktree-*` branch as the deliverable.

**Commit before you finish, always.** The commonest stranding here is not an unmerged branch — it
is edits that were flashed to hardware, confirmed working, and then left uncommitted in the
worktree. No branch-comparison tool can see those. Run `git status --porcelain` in the worktree
before reporting a task done; if it is not empty, commit and land it.

**Checking what is stranded: compare patch-ids, not SHAs.**

```bash
git cherry companion <branch>   # '+' = genuinely not on companion, '-' = already applied
```

`git log companion..<branch>` and `git rev-list --count companion..<branch>` are SHA-based, and
landing here is normally done by cherry-pick — which rewrites the SHA, so those commands report
every landed branch as unlanded forever. Uncommitted work needs a separate sweep:

```bash
git worktree list --porcelain | awk '/^worktree /{print substr($0,10)}' |
    while read -r p; do [ -n "$(git -C "$p" status --porcelain)" ] && echo "dirty: $p"; done
```

A `SessionStart` hook (`.claude/hooks/prune-worktree-bridges.sh`) runs both sweeps automatically
and reports anything holding uncommitted work or never landed. It never removes a dirty worktree.
It and `.claude/settings.json` are deliberately **untracked** — `.gitignore`'s `.claude/*` line is
upstream's, and negating it would be a carried patch that conflicts on every sync — so they must
be recreated by hand in a fresh clone. Note `git worktree remove` refuses outright on a repo with
submodules (`--force` does not override), so pruning is report-only here; stale worktrees run to
gigabytes each and need deleting by hand.

**A worktree's branch is a static fork point — it does not track `companion` moving forward.**
`companion` can (and does) gain commits after a worktree is provisioned but before that worktree's
session actually starts doing work — the worktree just sits at whatever `companion` looked like when
it forked, however long ago that was. This showed up for real on 2026-08-15: a worktree forked before
`c48a7818` landed on `companion` tried to land its own peer-store changes hours later and hit a
cherry-pick conflict against code it had never seen, in a file it had no reason to think anyone else
had touched. `prune-worktree-bridges.sh` does not catch this — it only runs from the main checkout,
explicitly skips locked (live-session) worktrees, and checks landed-ness, not currency.

`.claude/hooks/sync-worktree-bridge.sh` is the fix: a second `SessionStart` hook that runs *from
inside* a bridge worktree (the case the prune hook always skips), checks how far `HEAD` is behind
`companion`, and auto-merges when it's safe to (working tree clean, merge produces no conflicts).
When it isn't safe — dirty tree, or the merge itself conflicts — it reports the drift and leaves the
worktree untouched rather than forcing anything, so this is not a substitute for reading the output:
if it reports conflicts, resolve them (`git merge companion`) before relying on files it touches being
current. **This hook only fires if it and `.claude/settings.json` are present in the worktree's own
`.claude/` directory** — both are gitignored per-worktree same as in a fresh clone, and nothing
copies them there automatically today. If `.claude/hooks/sync-worktree-bridge.sh` is missing from a
worktree, copy both it and `prune-worktree-bridges.sh` plus `.claude/settings.json` from the main
checkout before starting substantive work — or at minimum run `git rev-list --count HEAD..companion`
by hand and merge if it's nonzero.

## Docs that are upstream's, not ours

`GOVERNANCE.md`, `docs/contributing/`, and `docs/translators.md` describe upstream's community
process. They are kept unmodified for clean merges. Do not cite them as this fork's process, and do
not update them to match this fork.

`ROADMAP.md`, `README.md`, `SCOPE.md`, and this file are fork-owned and marked `merge=ours` in
[.gitattributes](.gitattributes) — upstream's edits to them are dropped automatically. This requires
`git config merge.ours.driver true` once per clone.
