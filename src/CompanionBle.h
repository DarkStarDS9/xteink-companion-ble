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
// STATUS: interface only, no implementation yet. Written without a PlatformIO/
// ESP-IDF toolchain available to build against, so this header is a design
// contract to implement and verify on real hardware, not a working module.
// Do not add this to platformio.ini / main.cpp until CompanionBle.cpp exists,
// builds clean (`pio run`), and has been heap-profiled per the note below.

#include <cstddef>
#include <cstdint>

namespace companionble {

// Advertised local name while in Companion Mode.
inline constexpr const char* kDeviceName = "SpokenFeeds X3";

// NimBLE's own heap cost is ~52-70 KB measured on this same MCU by BleInput.h's
// kStartMinFreeHeap/kStartMinFreeHeapExplicit (HID-host role) — treat that as the
// starting estimate for the peripheral role too (same underlying NimBLE stack),
// re-measure with ESP.getFreeHeap() before/after ensureStarted() once this
// builds, and adjust the floor below to the measured number rather than trusting
// this estimate.
inline constexpr size_t kStartMinFreeHeap = 70 * 1024;

enum class ButtonEvent : uint8_t {
  Confirm = 0x01,
  Back = 0x02,
};

// Start advertising the Companion Display Protocol GATT service (idempotent).
// Follow BleInput::ensureStarted()'s pattern: wrap NimBLE init in
// HalPowerManager::Lock (NimBLE controller init hangs at the 10 MHz low-power
// clock — see BleInput.cpp's comment on this), and gate on
// ESP.getFreeHeap() >= kStartMinFreeHeap same as bleinput::ensureStarted() does,
// logging + returning false rather than starting under budget.
bool ensureStarted();
bool startInProgress();

// Full NimBLE teardown, mirroring bleinput::stop() — must return the stack's
// RAM to the heap, not just drop the link, for the same reason BleInput.cpp
// documents (EPUB inflate and other reader paths need that RAM back).
void stop();

bool isConnected();

// Push one field (title or body) to the connected central, framed per
// docs/companion-display-protocol.md (START declares field + total length,
// CHUNK carries payload sized to the negotiated MTU minus 1 byte framing
// overhead, END closes it). No-op if not connected. `field` is 0x01 for title,
// 0x02 for body, matching the protocol doc.
bool pushField(uint8_t field, const char* utf8, size_t len);

// Registers the callback invoked from the NimBLE host task when a button-event
// notification's subscription is live and the corresponding physical button
// (CONFIRM/BACK — see MappedInputManager::Button) is pressed while Companion
// Mode is the active activity. Runs on the NimBLE host task, not the main loop
// task — do only cheap, thread-safe work here (e.g. xQueueSend to hand off to
// the activity's loop()), per the ISR/task shared-state rules in CLAUDE.md.
using ButtonEventCallback = void (*)(ButtonEvent);
void setButtonEventCallback(ButtonEventCallback cb);

}  // namespace companionble
