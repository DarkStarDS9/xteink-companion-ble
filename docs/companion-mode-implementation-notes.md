# Companion Mode — implementation status

Implemented and verified on real X3 hardware (2026-07-22). See
`docs/companion-display-protocol.md` for the wire protocol and
`src/CompanionBle.h` for the interface.

## What's implemented

- `src/CompanionBle.cpp` — NimBLE GATT peripheral: service/characteristics,
  content reassembly (START/CHUNK/END), capability characteristic, button
  notify, heap-floor-gated `ensureStarted()`/`stop()`.
- `src/activities/companion/CompanionModeActivity.{h,cpp}` — waiting screen,
  pagination (reuses the reader's text-wrap measure-and-break loop), LEFT/RIGHT
  local paging, CONFIRM/BACK BLE notify.
- Settings menu entry (`SettingsActivity`).
- iOS side already implemented and TestFlight-deployed
  (`src/iOS/SpokenFeedsMixer/.../Services/CompanionDeviceService.swift`).

## Verified end-to-end (independent Mac `bleak` client, real X3 hardware)

Connect → read capability characteristic → push 47 content packets
(title+body, 834 chars) → device paginates and renders → clean disconnect →
reconnect → clean disconnect again. Heap stable (~40-45 KB free) across both
connect cycles, no leak.

**Not yet verified**: CONFIRM/BACK button-press notify round-trip (needs a
human physically pressing the device's buttons while a central is connected
and subscribed — not something a scripted test can drive). Code review
confidence is high (mirrors `BleKeyboardHost`'s already-tested notify
mechanics), but do a real press-and-confirm pass before considering this done.

## Bugs found and fixed during hardware bring-up

1. **Heap-floor mistuning.** First guess (70 KB, copied from `BleInput.h`'s
   HID-*host* figure) was replaced with a wrong 100 KB "fix" derived from a
   whole-session heap logger that conflated navigation overhead with NimBLE's
   own cost. `ensureStarted()` now logs `ESP.getFreeHeap()` immediately before
   `NimBLEDevice::init()` and right after `g_server->start()` — the isolated
   delta measured 64,600-64,724 bytes across multiple runs. Floor set to 80 KB
   (measured cost + ~16 KB margin).

2. **Advertising payload overflow.** A 128-bit service UUID (18 bytes) + the
   device name "SpokenFeeds X3" (16 bytes) + flags (3 bytes) = 37 bytes,
   over BLE's 31-byte legacy advertising PDU limit. Central-role scanning that
   filters by service UUID (`scanForPeripherals(withServices:)` on iOS) may
   never match if the UUID gets silently dropped/truncated to fit. Fixed by
   splitting the primary advertisement (service UUID only) from the scan
   response (device name), via `NimBLEAdvertisementData` +
   `setAdvertisementData()`/`setScanResponseData()`.

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
fix; already flagged in the iOS code's own comment.

## Remaining before merging to master

- Physical CONFIRM/BACK button-press verification with a connected central.
- Remove the temporary auto-enter-Companion-Mode-after-3s debug aid in
  `HomeActivity.{h,cpp}` (clearly marked, grep for "TEMPORARY DEBUG AID").
- Re-test pairing directly against the iOS app (verified independently via a
  scripted Mac BLE client above; the one iOS pairing attempt during this
  session predated the power-lock fix).
