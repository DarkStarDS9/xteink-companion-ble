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
NimBLE ~63 KB, framebuffer 48 KB. There is no room for a third large allocation.

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
