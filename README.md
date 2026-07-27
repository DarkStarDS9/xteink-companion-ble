# xteink-companion-ble

BLE companion-display firmware for Xteink e-ink hardware (X3/X4, ESP32-C3).

A fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) that
repurposes the reader firmware as a **phone-driven second screen**. The device boots straight into
`CompanionModeActivity` and exposes a BLE GATT peripheral; a paired phone app pushes content to it
and receives button events back.

This is not a general-purpose e-reader build. The reader, library, and EPUB rendering code is still
compiled in, but unreachable in normal operation.

## Consumers

The firmware is a **platform serving several apps**, not the device half of one product:

| App | Repo | Status |
|---|---|---|
| SpokenFeeds audio companion | `DarkStarDS9/SpokenFeeds` (iOS) | Shipped, protocol v5 |
| Polaroid camera app | not started | Design sketch |
| Offline article reader | undecided | Idea |

Anything app-specific lives in that app's repo. What lives here is the protocol, the on-device UI,
and the shared capability surface.

## Start here

| Doc | What it covers |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Where this fork is going, and why |
| [SCOPE.md](SCOPE.md) | What belongs in this firmware and what does not |
| [docs/companion-display-protocol.md](docs/companion-display-protocol.md) | **Authoritative** GATT wire protocol (v5) |
| [docs/companion-mode-implementation-notes.md](docs/companion-mode-implementation-notes.md) | On-device implementation notes |
| [docs/companion-image-protocol-sketch.md](docs/companion-image-protocol-sketch.md) | Draft image-push extension |
| [CLAUDE.md](CLAUDE.md) | Fork-specific engineering rules for AI agents |
| [.skills/SKILL.md](.skills/SKILL.md) | Shared engineering guide, inherited from upstream |

## Build

Standard PlatformIO. Requires the `freeink-sdk` submodule:

```bash
git submodule update --init --recursive
pio run                 # build
pio run -t upload       # flash
```

## Repository layout

This repo is the **canonical development workspace** — upstream syncs and protocol work happen
here. Consumer apps embed it as a git submodule and pin a commit; they do not develop in it.

Branches:

- **`companion`** — the trunk. Default branch. All work lands here.
- **`develop`** — a pristine mirror of `upstream/develop`. Never commit to it; only
  `git merge --ff-only upstream/develop`.
- **`upstream-fixes/*`** — one branch per PR sent back to CrossPoint, always cut from `develop`.

Syncing upstream in:

```bash
git fetch upstream
git checkout develop && git merge --ff-only upstream/develop && git push origin develop
git checkout companion && git merge develop
```

Merge — never rebase. Rebasing rewrites SHAs and strands the submodule pins in consumer repos.

## Inherited upstream docs

`GOVERNANCE.md`, `docs/contributing/`, and `docs/translators.md` describe **upstream CrossPoint's**
project process and community, not this fork's. They are kept unmodified so upstream's improvements
keep merging cleanly. This fork takes no outside contributions; fixes worth sharing go upstream as
PRs from `upstream-fixes/*`.

## Licence

Inherited from CrossPoint Reader — see [LICENSE](LICENSE).
