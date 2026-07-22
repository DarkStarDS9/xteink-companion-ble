#pragma once

// Companion Display Protocol — NimBLE GATT *peripheral* glue.
//
// Unlike BleInput.h/.cpp (BLE HID *host*, X3 as central connecting to a
// page-turner remote), this is the opposite BLE role: X3 as GATT *peripheral*,
// a phone app as central. The FreeInk SDK has no peripheral/GATT-server
// abstraction (only BleKeyboardHost's host role, see docs/ble-keyboard-host.md
// upstream) — this talks to NimBLE-Arduino's server APIs (NimBLEServer,
// NimBLEService, NimBLECharacteristic) directly.
//
// See docs/companion-display-protocol.md for the wire format this implements
// (service/characteristic UUIDs, START/CHUNK/END content framing, button-event
// byte values, capability characteristic layout).
//
// STATUS: implemented and verified on real X3 hardware, see
// docs/companion-mode-implementation-notes.md for the bring-up log and the
// heap-floor measurement this header's kStartMinFreeHeap note below cites.

#include <cstddef>
#include <cstdint>

class GfxRenderer;

namespace companionble {

// Advertised local name prefix while in Companion Mode. The full advertised
// name appends a short stable per-device suffix (see buildDeviceName() in
// CompanionBle.cpp) so two readers running this same generic firmware don't
// show up as two identical rows in a phone's BLE picker. Kept app-agnostic —
// this protocol/firmware doesn't assume a specific companion app.
inline constexpr const char* kDeviceNamePrefix = "CrossPoint Companion";

// Measured on real X3 hardware (2026-07-22, ESP32-C3, stock NimBLE footprint,
// no custom_sdkconfig trim — see platformio.ini's note on why): ensureStarted()
// logs ESP.getFreeHeap() immediately before NimBLEDevice::init() and right
// after g_server->start() (CBLE tag), isolating NimBLE's own cost from
// navigation/activity overhead. Measured delta: 99220 -> 34620 bytes, a
// 64600-byte (~63 KB) cost — confirmed stable afterward (steady low-heap
// reading held, no crash, no drift over the session). Floor set with ~16 KB
// margin above that measured cost.
inline constexpr size_t kStartMinFreeHeap = 80 * 1024;

// Max bytes buffered per field (title/body), independent of negotiated MTU —
// this bounds the reassembly buffer, not a single CHUNK packet. Advertised to
// clients via the capability characteristic's max-content-length field;
// content past this length is truncated per docs/companion-display-protocol.md.
inline constexpr uint16_t kMaxFieldLen = 4096;

// Wire protocol v2 (see docs/companion-display-protocol.md). Values match the
// button-event characteristic byte exactly — do not renumber without bumping
// the capability characteristic's protocol version.
enum class ButtonEvent : uint8_t {
  PlayPause = 0x01,
  Prev = 0x02,
  Next = 0x03,
  ReadLater = 0x04,
};

// Status characteristic values, phone -> device (see docs/companion-display-protocol.md).
enum class StatusEvent : uint8_t {
  ReadLaterSaved = 0x01,
};

// Field identifiers for ContentFieldCallback, matching docs/companion-display-protocol.md.
inline constexpr uint8_t kFieldTitle = 0x01;
inline constexpr uint8_t kFieldBody = 0x02;

// Start advertising the Companion Display Protocol GATT service (idempotent).
// Follow BleInput::ensureStarted()'s pattern: wrap NimBLE init in
// HalPowerManager::Lock (NimBLE controller init hangs at the 10 MHz low-power
// clock — see BleInput.cpp's comment on this), and gate on
// ESP.getFreeHeap() >= kStartMinFreeHeap same as bleinput::ensureStarted() does,
// logging + returning false rather than starting under budget.
//
// `renderer` and `fontId` are used once, synchronously, to compute the static
// capability characteristic (screen width/height in characters at the
// Companion Mode font, per root CLAUDE.md § Orientation-Aware Logic — never
// hardcode screen dimensions). No reference to `renderer` is retained.
bool ensureStarted(const GfxRenderer& renderer, int fontId);
bool startInProgress();

// Full NimBLE teardown, mirroring bleinput::stop() — must return the stack's
// RAM to the heap, not just drop the link, for the same reason BleInput.cpp
// documents (EPUB inflate and other reader paths need that RAM back).
void stop();

bool isConnected();

// Notify a button press (PLAY_PAUSE/PREV/NEXT/READ_LATER) to the connected
// central, if any. No-op if not connected. Call this directly from
// CompanionModeActivity::loop() after polling mappedInput.wasPressed(...) —
// same pattern every other Activity uses to read input (there is no
// host-task-originated button path to hand off: buttons are polled on the
// main loop task, and NimBLE's notify() is safe to call from any task).
// Returns false if not connected or the notify failed.
bool notifyButtonEvent(ButtonEvent event);

// Callback for a completed content field (title or body), fully reassembled
// from START/CHUNK/END frames. Registered via setContentFieldCallback() and
// invoked from the NimBLE host task's write callback — this direction DOES cross a
// task boundary (NimBLE host task -> main loop task), so implementations must
// only do cheap, thread-safe work here (e.g. copy into a lock-guarded buffer
// and set a flag CompanionModeActivity::loop() polls), per the ISR/task
// shared-state rules in root CLAUDE.md. `field` is 0x01 for title, 0x02 for
// body, matching the protocol doc. `data`/`len` are only valid for the
// duration of the call.
using ContentFieldCallback = void (*)(uint8_t field, const uint8_t* data, size_t len);
void setContentFieldCallback(ContentFieldCallback cb);

// Callback for a Status characteristic write (phone -> device), e.g. a
// READ_LATER_SAVED acknowledgement after a READ_LATER button notify. Same
// task-boundary rules as ContentFieldCallback: invoked from the NimBLE host
// task, must only do cheap thread-safe work (set a flag/value under a lock
// that loop() polls).
using StatusCallback = void (*)(uint8_t status);
void setStatusCallback(StatusCallback cb);

}  // namespace companionble
