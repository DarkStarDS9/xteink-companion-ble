# Companion Mode — implementation notes / TODO

Written without a PlatformIO/ESP-IDF toolchain available (no `pio` in this
environment, so nothing here has been built or flashed). This is a scoped
TODO list for whoever implements Companion Mode next, not a claim that it
works.

See `docs/companion-display-protocol.md` for the wire protocol and
`src/CompanionBle.h` for the interface contract this needs to fill in.

## Why this can't reuse `feat-bluetooth`'s BLE code

`src/BleInput.h`/`.cpp` on `feat-bluetooth` is a BLE HID **host** — the X3
acts as BLE *central*, pairing to page-turner remotes (external keyboards).
Companion Mode needs the opposite role: X3 as GATT *peripheral*, phone as
central. The FreeInk SDK (`freeink-sdk/libs/network/BleKeyboardHost/`) only
implements the host role — there's no peripheral/GATT-server helper to build
on. `CompanionBle` will need to talk to NimBLE-Arduino's server APIs
(`NimBLEServer`, `NimBLEService`, `NimBLECharacteristic`) directly, the same
underlying library `BleKeyboardHost` uses internally, just a different API
surface.

One thing *is* reusable from `feat-bluetooth`: proof that NimBLE fits in this
device's RAM budget, and the exact pattern for doing it safely —
`bleinput::kStartMinFreeHeap`/`kStartMinFreeHeapExplicit` gate `ensureStarted()`
on measured free heap before touching NimBLE, and `HalPowerManager::Lock`
wraps the init/deinit calls because NimBLE controller init hangs at the 10 MHz
low-power CPU frequency. `CompanionBle::ensureStarted()` should follow the
same two patterns.

## Remaining work

1. **`CompanionBle.cpp`** — implement the interface in `CompanionBle.h`:
   - `ensureStarted()`: heap-floor check, `HalPowerManager::Lock`, bring up
     `NimBLEServer` + one `NimBLEService` (UUID from the protocol doc) with
     the three characteristics, start advertising.
   - Content characteristic write callback: reassemble START/CHUNK/END frames
     into two buffers (title, body) sized to the capability characteristic's
     advertised max content length. Use `makeUniqueNoThrow<uint8_t[]>` per
     `lib/Memory/Memory.h`, not bare `new` (see root `CLAUDE.md` § Heap
     Buffer Allocation) — this runs on the NimBLE host task and OOM there
     should degrade gracefully, not `abort()`.
   - Button-event notify: call from `MappedInputManager`'s CONFIRM/BACK
     handling, but only while Companion Mode is the active activity (don't
     hijack these buttons globally).
   - Capability characteristic: static read value, protocol version + screen
     char-grid size (`renderer.getScreenWidth()`/`getScreenHeight()` divided
     by the Companion Mode font's advance width/line height, not hardcoded —
     see root `CLAUDE.md` § Orientation-Aware Logic) + max content length.
   - `stop()`: full NimBLE deinit, mirroring `bleinput::stop()`, so the ~52-70
     KB comes back for EPUB/reader work when Companion Mode exits.

2. **`src/activities/companion/CompanionModeActivity.{h,cpp}`** — new
   `Activity` subclass (see `src/activities/Activity.h`, and
   `src/activities/reader/TxtReaderActivity.h` as the closest existing
   pattern for the pagination piece):
   - `onEnter()`: `companionble::ensureStarted()`, show a "waiting for
     connection" screen until `isConnected()`.
   - On content received (register via `setButtonEventCallback`-style
     handoff, or poll a flag set from the BLE callback — cross the NimBLE
     host task → main loop task boundary with a queue/flag per the ISR/task
     rules in root `CLAUDE.md`, not a raw shared struct): paginate the body
     text into `linesPerPage`/`pageOffsets`-style chunks like
     `TxtReaderActivity` does, using the renderer's existing text-wrap path
     rather than reimplementing wrapping.
   - `loop()`: LEFT/RIGHT move `currentPage` and re-render locally, no BLE
     traffic. CONFIRM/BACK call `companionble`'s notify path instead of any
     local action.
   - `onExit()`: `companionble::stop()`.
   - Use partial/grayscale refresh for page turns (`renderer`'s grayscale
     refresh path, ~127 ms per the hardware specs in the main protocol doc),
     not full refresh.

3. **Menu wiring** — add a "Companion Mode" entry to Home/Settings
   (`src/activities/settings/SettingsActivity.cpp` shows the pattern
   `feat-bluetooth` used to add `BluetoothSettingsActivity` at line ~63/300).
   Not done here — deliberately left out until step 1-2 build clean, so
   nothing half-working is reachable from the menu.

4. **`platformio.ini`** — add the NimBLE-Arduino dependency (or confirm it's
   already pulled transitively via `BleKeyboardHost`'s `library.json` — check
   before adding a duplicate). `feat-bluetooth`'s `platformio.ini` diff is the
   reference for what changed there.

## Verification (do this before considering it done)

- `pio run` clean build, `pio check` (cppcheck) clean.
- `ESP.getFreeHeap()` logged immediately before and after
  `companionble::ensureStarted()`, on real hardware — replace the `70 * 1024`
  estimate in `CompanionBle.h` with the measured number.
- Manual device test: connect, push a body long enough to need 3+ pages, page
  through with LEFT/RIGHT, press CONFIRM and BACK and confirm a connected
  central sees the notify, disconnect/reconnect, and check heap doesn't creep
  down over repeated connect/disconnect cycles (leak check).
