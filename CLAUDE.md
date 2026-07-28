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
- Field ids live in [src/CompanionBle.h](src/CompanionBle.h). Shipped: `0x01` title, `0x02` body,
  `0x03` content-id. Reserved by design docs, not yet implemented: `0x04` image, `0x05` button map,
  `0x06` icon. Next free: `0x07`.
- The planned v6 shape — sessions, phone:app pairing, per-peer SD storage, button labels/routing —
  is in [docs/companion-multi-app-design.md](docs/companion-multi-app-design.md). It is a **clean
  break**: v6 requires a handshake, so shipped v5 clients stop working until updated.

## Upstream relationship

- **`companion`** is the trunk and default branch. **`develop`** is a pristine mirror of
  `upstream/develop` — never commit to it.
- Sync by **merging**, never rebasing: rebasing rewrites SHAs and strands submodule pins in consumer
  repos.
- **Generic fixes go upstream**, not into the fork — crashes, driver bugs, leaks, formatting. Use an
  `upstream-fixes/*` branch cut from `develop`. Carried patches are a recurring merge cost.
- The boot path in `src/main.cpp` is the one deliberate divergence and conflicts on most syncs.
  Keep the companion side.

## Docs that are upstream's, not ours

`GOVERNANCE.md`, `docs/contributing/`, and `docs/translators.md` describe upstream's community
process. They are kept unmodified for clean merges. Do not cite them as this fork's process, and do
not update them to match this fork.

`ROADMAP.md`, `README.md`, `SCOPE.md`, and this file are fork-owned and marked `merge=ours` in
[.gitattributes](.gitattributes) — upstream's edits to them are dropped automatically. This requires
`git config merge.ours.driver true` once per clone.
