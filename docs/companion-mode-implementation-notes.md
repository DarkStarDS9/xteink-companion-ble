# Companion Mode — implementation status

Implemented and verified on real X3 hardware (2026-07-22, v1/v2). v3 (this
session) build-verified only — see "Remaining before merging to master". See
`docs/companion-display-protocol.md` for the wire protocol (now v3) and
`src/CompanionBle.h` for the interface.

## What's implemented

- `src/CompanionBle.cpp` — NimBLE GATT peripheral: service/characteristics,
  content reassembly (START/CHUNK/END, now including the content-id field
  0x03), capability characteristic (protocol v3), button notify
  (PLAY_PAUSE/PREV/NEXT/READ_LATER, now with the last-pushed content-id bytes
  appended to every notification), Status characteristic (READ_LATER_SAVED),
  heap-floor-gated `ensureStarted()`/`stop()`. Advertised name is a generic
  prefix + a short eFuse-MAC-derived suffix, not a fixed string, so multiple
  devices don't collide in a phone's BLE picker.
- `src/activities/companion/CompanionModeActivity.{h,cpp}` — waiting screen,
  pagination (reuses the reader's text-wrap measure-and-break loop), bottom
  LEFT/RIGHT local paging (page-turn hints shown only when that direction is
  actually pageable), side UP/DOWN → BLE PREV/NEXT (behind the
  `kSideUpMeansPrev` swap constant, no on-screen hint drawn for these two
  buttons), bottom BACK → PLAY_PAUSE, bottom CONFIRM → READ_LATER, bold
  larger title that wraps onto up to 2 lines (falling back to ellipsis
  truncation only if still too long) with a hand-drawn read-later star
  (outline/filled, flipped by the Status write), bottom button-hint bar,
  idle-sleep timeout on the waiting screen (`kWaitingIdleSleepMs`).
- The device now boots directly into Companion Mode (`main.cpp`) — the
  firmware is companion-only in normal operation. Recovery firmware mode
  (UP+POWER at boot) and crash-report-after-panic are the only other boot
  targets; Home/reader activities are still compiled (reachable only via
  `SettingsActivity`'s old Companion Mode entry point and other now-dead
  internal paths) but are not reached from a normal boot or from within
  Companion Mode's own button routing.
- iOS side (`src/iOS/SpokenFeedsMixer/.../Services/CompanionDeviceService.swift`)
  is being updated alongside this v3 firmware change (content-id push,
  content-id comparison before acting on a button event, re-push on
  reconnect) — see that repo's own history for the matching commit.

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
- v3 content-id round-trip on real hardware: push a content-id, press a
  mapped button, confirm the phone receives it appended after the event byte;
  push a *different* content-id and confirm a stale phone-side comparison
  would reject the next press. Title-wrap (up to 2 lines) and the
  side-UP/DOWN hint removal also need on-device visual confirmation — both
  were only exercised via `pio run` build-verification, not rendered on a
  real panel yet.

---

## v6 bring-up log (2026-07-28)

Protocol v6 (sessions, pairing tokens, per-peer SD storage, button map, icons,
image push) implemented and flashed to a real X3. `docs/companion-display-protocol.md`
is authoritative for the wire format; this section records only what has and has
not been observed on hardware.

### Observed on hardware

- Builds and flashes clean. RAM 68,188 B (20.8%, +464 B over the v5 baseline of
  67,724), flash 5,758,937 B (87.9%, +14.3 KB). The +464 B is the session table
  and the activity's new state, and is inside the design's 1 KB net-new target —
  everything per-peer lives on SD.
- Cold boot enters `CompanionModeActivity` directly and starts advertising:
  `advertising: start()=1 isAdvertising()=1`, service UUID byte-verified in the
  advertisement payload.
- NimBLE cost measured again on this build: heap 124,168 → 58,980 at server
  start, i.e. 65,188 bytes. Consistent with the 64,600 measured for v5 — the
  session layer did not move it.
- Free heap steady at 50,948 with min-free 50,932 over several minutes idle on
  the waiting screen. No drift, no crash.

### Not yet verified — needs a BLE central

Everything below is build-verified only. The dev-machine pusher
(`scripts/push_companion_content.py`, rewritten for v6) is the intended path and
implements all of it, but macOS refused Bluetooth permission to the automation
process that flashed this build, so nothing on the wire has been exercised. Run
it from a terminal that has been granted Bluetooth access:

```
pip install bleak Pillow
python scripts/push_companion_content.py            # pair, push text
python scripts/push_companion_content.py --listen   # button events
python scripts/push_companion_content.py --image-from photo.jpg
python scripts/push_companion_content.py --icon icon.png --no-text
python scripts/push_companion_content.py --forget   # force a fresh pairing prompt
```

The full list of what to check is the "Manual verification checklist" at the end
of `docs/companion-display-protocol.md` — 36 items covering sessions, pairing,
button-map gating, content, images, icons and power. In particular these three
carry real risk and have no build-time signal at all:

1. **SD access from the NimBLE host task.** `HELLO` reads/writes `peers.json`
   and the token file, and image CHUNKs are appended to SD, all from NimBLE's
   host task rather than the main loop. That task's stack is NimBLE-Arduino's
   default 4 KB. The SD paths here are shallow (no recursion, JSON parsing is
   heap-backed) and this codebase already writes SD from the web-server task,
   but a stack overflow would show as a crash during pairing or mid-image, not
   as anything a build catches. Watch for `Stack canary watchpoint` on serial.
2. **Image decode path with `useDithering = false`.** The two-pass grayscale
   settle re-decodes the staged PNG twice more via `ReaderUtils::renderAntiAliased`.
   Confirm free heap during a push stays near its idle value — if it dips by
   anything image-sized, something is buffering that should not be.
3. **The icon grid's 1-bpp blit.** Row padding and MSB-first bit order are
   written to the documented contract but have never been drawn.

### Rolling back to v5

Flashing v6 disconnects any shipped v5 client until that client ships its v6
update. The last v5 firmware commit is **`5688aec6`**, and it rebuilds from a
clean tree:

```
git stash                       # if you have work in progress
git checkout 5688aec6
pio run -t upload --upload-port /dev/cu.usbmodem21201
git checkout companion
git stash pop
```

No prebuilt binary is kept in the repo — a 5.7 MB artifact in git for a
two-minute rebuild is not a trade worth making, and a stale one is worse than
none.

### Deliberately not carried forward from v5

The v5 "Remaining before merging to master" list above is obsolete: its button
mapping (side UP/DOWN → PREV/NEXT, `kSideUpMeansPrev`) was removed in v5 itself,
and its remaining items were closed. The v6 button behaviour is not a firmware
decision at all — it comes from the foreground app's pushed button map.
