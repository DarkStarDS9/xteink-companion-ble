# Companion Mode — implementation status

Implemented and verified on real X3 hardware (2026-07-22). See
`docs/companion-display-protocol.md` for the wire protocol (now v2) and
`src/CompanionBle.h` for the interface.

## What's implemented

- `src/CompanionBle.cpp` — NimBLE GATT peripheral: service/characteristics,
  content reassembly (START/CHUNK/END), capability characteristic (protocol
  v2), button notify (PLAY_PAUSE/PREV/NEXT/READ_LATER), Status characteristic
  (READ_LATER_SAVED), heap-floor-gated `ensureStarted()`/`stop()`. Advertised
  name is a generic prefix + a short eFuse-MAC-derived suffix, not a fixed
  string, so multiple devices don't collide in a phone's BLE picker.
- `src/activities/companion/CompanionModeActivity.{h,cpp}` — waiting screen,
  pagination (reuses the reader's text-wrap measure-and-break loop), bottom
  LEFT/RIGHT local paging, side UP/DOWN → BLE PREV/NEXT (behind the
  `kSideUpMeansPrev` swap constant), bottom BACK → PLAY_PAUSE, bottom CONFIRM
  → READ_LATER, bold larger title with a hand-drawn read-later star
  (outline/filled, flipped by the Status write), button-hint bar + side
  hints, idle-sleep timeout on the waiting screen (`kWaitingIdleSleepMs`).
- The device now boots directly into Companion Mode (`main.cpp`) — the
  firmware is companion-only in normal operation. Recovery firmware mode
  (UP+POWER at boot) and crash-report-after-panic are the only other boot
  targets; Home/reader activities are still compiled (reachable only via
  `SettingsActivity`'s old Companion Mode entry point and other now-dead
  internal paths) but are not reached from a normal boot or from within
  Companion Mode's own button routing.
- iOS side already implemented and TestFlight-deployed
  (`src/iOS/SpokenFeedsMixer/.../Services/CompanionDeviceService.swift`) —
  **not yet updated for the v2 button codes / Status characteristic** as of
  this firmware change; needs a matching iOS update before end-to-end testing
  the new mapping.

## Verified end-to-end (independent Mac `bleak` client, real X3 hardware, protocol v1)

Connect → read capability characteristic → push 47 content packets
(title+body, 834 chars) → device paginates and renders → clean disconnect →
reconnect → clean disconnect again. Heap stable (~40-45 KB free) across both
connect cycles, no leak.

**Not yet re-verified on hardware after this session's changes**: the v2
button-event codes, the Status characteristic round-trip, the read-later icon
flip, the new boot-direct-into-Companion-Mode flow, and the idle-sleep
timeout. This session's changes were made and build-verified (`pio run`)
without hardware access — see the "Remaining before merging to master"
section below.

## Bugs found and fixed during hardware bring-up (protocol v1, prior session)

1. **Heap-floor mistuning.** First guess (70 KB, copied from `BleInput.h`'s
   HID-*host* figure) was replaced with a wrong 100 KB "fix" derived from a
   whole-session heap logger that conflated navigation overhead with NimBLE's
   own cost. `ensureStarted()` now logs `ESP.getFreeHeap()` immediately before
   `NimBLEDevice::init()` and right after `g_server->start()` — the isolated
   delta measured 64,600-64,724 bytes across multiple runs. Floor set to 80 KB
   (measured cost + ~16 KB margin).

2. **Advertising payload overflow.** A 128-bit service UUID (18 bytes) + the
   device name + flags (3 bytes) can overflow BLE's 31-byte legacy
   advertising PDU. Central-role scanning that filters by service UUID
   (`scanForPeripherals(withServices:)` on iOS) may never match if the UUID
   gets silently dropped/truncated to fit. Fixed by splitting the primary
   advertisement (service UUID only) from the scan response (device name),
   via `NimBLEAdvertisementData` + `setAdvertisementData()`/`setScanResponseData()`.

3. **CPU low-power mode killed connection establishment (the real blocker).**
   `ensureStarted()` originally wrapped only `NimBLEDevice::init()` in a scoped
   `HalPowerManager::Lock`, matching `BleInput.cpp`'s pattern. But
   `HalPowerManager`'s idle logic drops the CPU to its low-power clock after a
   period of no screen activity — exactly what happens sitting on "Waiting for
   phone" with no further redraws — and NimBLE's connection-establishment
   processing on the host task hangs at that low clock, the same WDT-hang
   class documented for init/deinit specifically, but it turns out to also
   affect live connection negotiation. Symptom: advertising was running and
   independently discoverable (byte-verified UUID), but no central ever
   completed a connection unless it happened to arrive during a brief
   normal-frequency window (e.g. right after a screen refresh). Fixed by
   holding the `HalPowerManager::Lock` for the entire BLE session
   (`ensureStarted()` success through `stop()`, via a `g_powerLock` member),
   not just around init. Confirmed by reproducing the hang on real hardware
   (Mac `bleak` client's `connect()` timed out after 10s with zero `central
   connected` log) and then confirming the fix (immediate connect, full
   protocol round-trip, clean reconnect).

## Known non-firmware issue (iOS side, already documented in that code)

`SettingsView.swift`'s Companion Display status text doesn't live-update while
the sheet is open (nested `@Published` not observed) — close and reopen
Settings to see a fresh connection state. Not something this session needed to
fix; already flagged in the iOS code's own comment. Now also stale in that the
Settings-based entry point into Companion Mode is no longer how a normal boot
reaches it.

## Remaining before merging to master

- Physical button-press verification with a connected central, for the new
  v2 mapping: side UP/DOWN → PREV/NEXT, bottom BACK → PLAY_PAUSE, bottom
  CONFIRM → READ_LATER, bottom LEFT/RIGHT local paging only (no BLE event).
  Confirm `kSideUpMeansPrev` is the right polarity on real hardware — flip
  the one constant if not.
- Status characteristic (READ_LATER_SAVED) round-trip and the read-later icon
  flip, on-device — no scripted test can exercise the physical star glyph.
- Boot-direct-into-Companion-Mode flow, cold boot and warm/silent-reboot
  paths (`main.cpp`) — verify no stall/crash now that Home/reader are no
  longer reachable from normal boot routing.
- Idle-sleep timeout on the waiting screen (`kWaitingIdleSleepMs`, currently
  5 minutes) — verify the device actually deep-sleeps and that
  `HalPowerManager::startDeepSleep()` cleanly tears down BLE (deep sleep is a
  full chip reset, so this is expected to work, but hasn't been observed on
  hardware from this specific code path).
- iOS app update for the v2 button codes and Status characteristic — the
  currently-deployed iOS build still expects v1's CONFIRM/BACK codes and has
  no Status-characteristic write path.
