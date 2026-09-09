---
name: scope-discipline
description: Feature-scope discipline for a BLE companion-display firmware serving multiple consumer apps. Use when adding a feature, a new activity, a new lib, a setting, or a dependency, or when a request would grow the firmware's surface. Covers the SCOPE.md test, the dumb-firmware/smart-phone split, the RAM gate on a no-PSRAM ESP32-C3, whether a change belongs here or upstream, and how to push back on out-of-scope asks.
---

# Scope Discipline

The mission: be a **low-power, phone-driven second screen** — a platform serving several consumer
apps, not the device half of one product. `SCOPE.md` is the source of truth. Read it before adding
surface.

> This fork's scope is **not** upstream CrossPoint's. Upstream is a dedicated e-reader and bans
> interactive apps and news aggregators; those are this fork's purpose. Ignore upstream scope
> reasoning found in inherited docs.

## The gate

Before adding a feature, activity, lib, setting, or dependency, answer in order:

1. **Does it belong on the phone instead?** Default answer is yes. The device records raw events and
   renders what it is told; interpretation, heuristics, and content decisions are phone-side. If it
   can live on the phone, it does.
2. **Would more than one consumer app use it?** Protocol, companion UI, and power behaviour are
   shared and belong here. A capability only one app would ever use belongs in that app's repo.
3. **Where does the memory come from?** ~380 KB, no PSRAM, and NimBLE (~63 KB) plus the framebuffer
   (48 KB) are already committed. A new large buffer needs a concrete answer — usually "stream it to
   an SD scratch file". Quantify; do not guess.
4. **Should this go upstream instead?** Generic fixes — crashes, driver bugs, leaks, formatting —
   belong in an `upstream-fixes/*` PR, not the fork. Carried patches are a recurring merge cost.
5. **Can it be done with no new code?** Prefer an existing activity, setting, or doc. The cheapest
   feature is the one already built.

If a request fails the gate, push back with the specific reason and the `SCOPE.md` basis, and offer
the in-scope alternative. Make the call and say why; do not hand over a menu.

## The protocol is the product

Consumer apps depend on the wire format. Changing framing, field ids, or the capability
characteristic is a **breaking change**: bump the version and update
`docs/companion-display-protocol.md` in the same commit. Never let the doc and
`src/CompanionBle.h` disagree.

## Surface awareness

The firmware still carries upstream's dozens of reader activities, compiled in but unreachable.
Do not revive them casually — a `ROADMAP.md` item must call for it explicitly. Each reachable
activity is permanent RAM, permanent maintenance, and another thing every upstream merge must not
break.

## Settings are not free

A new setting is a field to persist, migrate, validate, translate, and render, plus combinatorial
test burden. Add one only when users genuinely need the choice; otherwise pick a sensible default.
Prefer negotiating behaviour over the protocol rather than storing it on-device.

## Self-review

- [ ] Checked against `SCOPE.md`; not on the out-of-scope list.
- [ ] Confirmed it cannot reasonably live on the phone.
- [ ] More than one consumer app would use it — or it is protocol/UI/power surface.
- [ ] Named the RAM cost (measured, not guessed) and where the memory comes from.
- [ ] Confirmed it is not a generic fix that should go upstream instead.
- [ ] Protocol change (if any) bumps the version and updates the protocol doc.
- [ ] New setting (if any) is justified by a real user need, not added "just in case."
