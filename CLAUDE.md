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

- Changing framing, field ids, or the capability characteristic is a **breaking change** — bump the
  version and update the doc in the same commit.
- Field ids live in [src/CompanionBle.h](src/CompanionBle.h). Implemented: `0x01` title, `0x02` body,
  `0x03` content-id, `0x04` image (raw packed 2bpp, full screen, no header — see
  `RawBitmapToFramebufferConverter` and the protocol doc's "Image field" section; this replaced an
  earlier PNG-based format, dropped because PNGdec's ~44 KB working set never fit alongside NimBLE
  on this part), `0x05` UI declaration, `0x06` icon, `0x07` tag state. Next free: `0x08`.
- The v6 shape — sessions, phone:app pairing, per-peer SD storage, button labels/routing — is in
  [docs/companion-multi-app-design.md](docs/companion-multi-app-design.md) and implemented in
  firmware, though unproven on real hardware (see the protocol doc's status warning). It was a
  **clean break**: v6 requires a handshake, so a v5 client gets no error, just silence.

## Upstream relationship

- **`companion`** is the trunk and default branch. **`develop`** is a pristine mirror of
  `upstream/develop` — never commit to it.
- Sync by **merging**, never rebasing: rebasing rewrites SHAs and strands submodule pins in consumer
  repos.
- **Generic fixes go upstream**, not into the fork — crashes, driver bugs, leaks, formatting. Use an
  `upstream-fixes/*` branch cut from `develop`. Carried patches are a recurring merge cost.
- The boot path in `src/main.cpp` is the one deliberate divergence and conflicts on most syncs.
  Keep the companion side.

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

## Docs that are upstream's, not ours

`GOVERNANCE.md`, `docs/contributing/`, and `docs/translators.md` describe upstream's community
process. They are kept unmodified for clean merges. Do not cite them as this fork's process, and do
not update them to match this fork.

`ROADMAP.md`, `README.md`, `SCOPE.md`, and this file are fork-owned and marked `merge=ours` in
[.gitattributes](.gitattributes) — upstream's edits to them are dropped automatically. This requires
`git config merge.ours.driver true` once per clone.
