# Project Vision & Scope: xteink-companion-ble

> This replaces upstream CrossPoint Reader's `SCOPE.md`. Upstream's scope is a **dedicated
> e-reader**; this fork's is a **companion display**. Several things upstream lists as out-of-scope
> (interactive apps, news aggregators) are the entire point here. Do not apply upstream's scope test
> to this repo.

## 1. Core Mission

Turn Xteink e-ink hardware into a **low-power, phone-driven second screen**. The phone does the
thinking; the device renders what it is told and reports what the user pressed.

The firmware is a **platform serving multiple consumer apps** (see [ROADMAP.md](ROADMAP.md)), not
the device half of a single product. That is the central scoping fact: a feature that serves only
one app usually belongs in that app, not here.

## 2. Guiding Principle: Dumb Firmware, Smart Phone

When behaviour could live on either side, it goes on the phone. The phone has memory, CPU, a real
toolchain, and ships updates in days; the device has 380 KB of RAM and needs a physical reflash.

Applied consistently, this means:

* The device records **raw events** (button id, hold duration, dwell time). It does not interpret
  them. "Read vs skimmed", "favourited", "play vs pause" are phone-side semantics — this is exactly
  why protocol v5 replaced semantic button actions with raw button identity.
* Image dithering, text layout decisions, and content selection happen on the phone.
* The device holds no state the phone cannot reconstruct, except an append-only event log awaiting
  sync.

## 3. The Hard Constraint

ESP32-C3, single-core RISC-V, **~380 KB RAM, no PSRAM**. The budget is already largely committed:
NimBLE ~63 KB (measured), framebuffer ~51 KB (52,272 bytes for the measured 528x792 panel — not the
48 KB some inherited notes assume). There is no room for a third large allocation.

Every proposal must answer: **where does the memory come from?** "Stream it to SD" is usually the
right answer — see the image-push design in [ROADMAP.md](ROADMAP.md), which streams BLE chunks to a
scratch file precisely because a RAM buffer will not fit.

## 4. Scope

### In-Scope

* **The BLE protocol.** Fields, framing, chunking, capability negotiation, versioning. This is the
  fork's primary product and the one thing no consumer app can own.
* **On-device companion UI.** What is rendered while paired, idle, connecting, or transferring.
* **Power behaviour.** Sleep, wake, idle timeout, USB-host detection, battery reporting.
* **Multi-consumer capabilities.** Image push, bulk transfer, device→phone sync — anything more than
  one consumer app could plausibly use.
* **Memory, flash, and code quality.** Refactors that reduce resource use or ease upstream merges,
  even with no user-visible change.
* **On-target test infrastructure.** A debug-build-only serial surface that lets a host drive the
  device alongside BLE ([docs/companion-test-console.md](docs/companion-test-console.md)), and the
  harness built on it. In scope because v6 enrollment needs a physical button press, so without it
  the protocol's most important path cannot be tested by any consumer app either — this is shared
  infrastructure, not one app's convenience. It is compiled out of every shipping build and may
  never expose an action a phone cannot perform over BLE; the moment it does, the tested path stops
  being the shipped path.

### Out-of-Scope

* **Reader features.** EPUB parsing, typography, hyphenation, library management. That is upstream's
  mission and it does it better. The code is compiled in but unreachable; leave it that way unless
  a roadmap item explicitly revives it.
* **Single-consumer features.** If only one app would ever use it, and it can live phone-side, it
  belongs in that app's repo.
* **On-device intelligence.** Interpretation, heuristics, and content decisions belong on the phone
  (§2).
* **Anything without a memory answer.** See §3.
* **Independent network connectivity.** The device talks to a paired phone over BLE. It does not
  fetch from the internet itself — no Wi-Fi sync engines, no cloud clients. Background radio work
  costs battery and RAM this device does not have.

## 5. Relationship to Upstream

This fork tracks `crosspoint-reader/crosspoint-reader` and merges from it regularly.

**If a fix is generic — a crash, a driver bug, a memory leak, a formatting issue — send it upstream**
as a PR from an `upstream-fixes/*` branch rather than keeping it forked. Every carried patch is a
recurring merge cost; every merged patch is maintained by someone else.

Keep a change forked only when it depends on companion-mode behaviour that upstream would reject.
Today that is essentially one thing: the boot path in `src/main.cpp`, which enters
`CompanionModeActivity` instead of routing to Home or the reader.

## 6. Ecosystem — Why This Base

*Surveyed 2026-07-27. Recorded so the "should we rebase onto something else" question does not get
re-litigated from scratch.*

CrossPoint Reader is **not itself a fork** — it is an original project (MIT, ~6.5k stars, ~1280
forks). Two families sit around it:

* **CrossPoint forks.** All reader variants: fonts and stats (`uxjulia/CrossInk`), reading
  consistency (`franssjz/cpr-vcodex`), a virtual pet (`trilwu/crosspet`), localizations
  (ko/jp/vi/th), genre readers (`SiliconAves/AvesO3`, `eszter007/matcha-reader`).
* **The `Free-Ink` org** — the layer *beneath* CrossPoint: `freeink-sdk` (the submodule this repo
  already builds against), plus minimal apps built directly on it: `freeink-reader`, `inkdeck`
  (markdown writer), `sticky-reminders`.

**No project in either family uses the device as a phone-driven BLE peripheral.** This niche is
unoccupied; we are not duplicating anyone's work.

### Why not build directly on `freeink-sdk`

That is the only serious alternative — `sticky-reminders` and `inkdeck` show a non-reader app on the
bare SDK works, and it would shed the unreachable reader code. Rejected because CrossPoint links
only the SDK's hardware, display, and UI libs — **not `FreeInkBook`** — so the pieces this fork
actually depends on are CrossPoint's own code, not the SDK's:

* `lib/GfxRenderer` — 4-level grayscale via two-pass overlay
* `lib/Epub/Epub/converters/` — PNG/JPEG → framebuffer plus `applyBayerDither4Level()`
* UITheme and themes, the activity framework, `PersistableStore`, 24-language i18n, the HAL

The image pipeline is exactly what the Polaroid roadmap item reuses. The SDK ships its own
(`FreeInkBook` with `pngle`), so a port is *possible* — but it means re-porting to an API CrossPoint
does not use, plus re-homing `CompanionBle` and `CompanionModeActivity`, for a firmware that already
works.

**Revisit only if** flash headroom becomes binding. Flash sits around 88% while RAM — the actual
constraint — sits near 21%, and unreachable code costs flash, not RAM. Trim in place first
(`OMIT_FONTS`, dropping unused activities) before considering a base change.

### Prior art worth reading, not forking

* [`0x1abin/crossmux`](https://github.com/0x1abin/crossmux) — apps hub: mini-games, tools, standby faces
* [`zakerytclarke/crosspoint-reader-apps`](https://github.com/zakerytclarke/crosspoint-reader-apps) — app support framework

Both solved **multi-mode selection on one device**, which is the open UI question in the Polaroid
roadmap item and a prerequisite for re-wiring the boot path. Read how they did it before designing
ours.
