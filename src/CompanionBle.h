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

// Max bytes retained for the content-id field (kFieldContentId) — deliberately
// much smaller than kMaxFieldLen. This is an opaque, client-defined
// correlation token (device never interprets it), appended verbatim to every
// button-event notification (see notifyButtonEvent()) so a client can detect
// "the button was pressed for a since-replaced article" and no-op instead of
// acting on stale state. Kept small on purpose: the button-event
// characteristic's notify payload is bounded by (negotiated MTU - 3 bytes ATT
// overhead) minus the 3-byte header+duration prefix (see notifyButtonEvent()),
// and other/future clients on this app-agnostic protocol may negotiate a
// much smaller MTU than this firmware's
// own 185 (BLE's guaranteed floor is MTU 23, i.e. 19 usable bytes worst
// case) — see docs/companion-display-protocol.md for the full budget math.
// A push exceeding this length is truncated, exactly like kMaxFieldLen for
// title/body.
inline constexpr size_t kMaxContentIdLen = 32;

// Wire protocol v5 (see docs/companion-display-protocol.md). Raw physical
// button identity — mirrors HalGPIO::BTN_*/InputManager::BTN_* exactly (do
// not renumber independently of those). Unlike the v2-v4 ButtonEvent enum
// this replaced (PLAY_PAUSE/PREV/NEXT/READ_LATER), the firmware assigns no
// meaning to a button beyond its physical identity — interpretation is
// entirely up to the client app. LEFT/RIGHT/POWER never reach BLE (see
// notifyButtonEvent()'s doc comment) but keep their HAL-matching values here
// so this enum stays a straight mirror of the HAL rather than a subset.
enum class ButtonId : uint8_t {
  Back = 0,
  Confirm = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  Power = 6,
};

// Button-event characteristic event types (top 3 bits of the header byte,
// see notifyButtonEvent()). Only one exists today; the field is reserved so
// a future non-press event (e.g. a heartbeat) could share this
// characteristic without a wire-incompatible change.
enum class ButtonEventType : uint8_t {
  ButtonPress = 0x01,
};

// Status characteristic values, phone -> device (see docs/companion-display-protocol.md).
enum class StatusEvent : uint8_t {
  ReadLaterSaved = 0x01,
};

// Field identifiers for ContentFieldCallback, matching docs/companion-display-protocol.md.
inline constexpr uint8_t kFieldTitle = 0x01;
inline constexpr uint8_t kFieldBody = 0x02;

// Wire protocol v4: the top bit of a START packet's field byte marks "this is the last field of
// an atomic content push". The device buffers each field's END as before, but only commits
// (applies + redraws) everything gathered so far once it processes an END whose START carried
// this bit — see the "Atomic multi-field pushes" section of docs/companion-display-protocol.md.
// Kept out of the low 7 bits used for field identity so `data[1] & kFieldMask` recovers the
// field id regardless of the flag.
inline constexpr uint8_t kFinalFieldFlag = 0x80;
inline constexpr uint8_t kFieldMask = 0x7F;

// Opaque client-defined correlation token (see kMaxContentIdLen's comment).
// Pushed via the same START/CHUNK/END framing as title/body, and fires
// ContentFieldCallback like title/body do — but CompanionModeActivity ignores
// this field id for on-screen state (nothing displayed depends on it) and
// only reacts to its `final` flag, to commit a pending title/body batch. The
// bytes themselves are handled entirely inside CompanionBle.cpp (remembered
// internally to echo back from notifyButtonEvent()).
inline constexpr uint8_t kFieldContentId = 0x03;

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

// Notify a button-press event to the connected central, if any. No-op if not
// connected. Call this from CompanionModeActivity::loop() — the caller owns
// all hold-tracking (first-press vs. repeat-while-held vs. release): this
// function just serializes whatever it's given. Buttons are polled on the
// main loop task, and NimBLE's notify() is safe to call from any task.
// Returns false if not connected or the notify failed.
//
// `durationTicks` is elapsed hold time in 100ms units since the initial press
// (0 for the initial-down event); `isFinal` marks the release event — the
// last one for this press/hold/release sequence. A client MUST NOT rely on
// `isFinal` alone to detect release (a disconnect mid-hold means it may never
// arrive) — treat "no repeat tick for noticeably longer than 100ms" as an
// implicit release too. See docs/companion-display-protocol.md.
//
// The notification payload (v5) is `[header byte] + [duration, uint16 LE] +
// [last-pushed content-id bytes]`. The header byte packs isFinal (bit 7),
// event type (bits 6-4, currently always ButtonEventType::ButtonPress), and
// the button id (bits 3-0) — see docs/companion-display-protocol.md for the
// exact bit layout. The caller does not pass the content-id; it's read
// internally from whatever kFieldContentId push landed most recently (empty
// if none yet).
bool notifyButtonEvent(ButtonId button, uint16_t durationTicks, bool isFinal);

// Callback for a completed content field (title, body, or content-id), fully reassembled
// from START/CHUNK/END frames. Registered via setContentFieldCallback() and
// invoked from the NimBLE host task's write callback — this direction DOES cross a
// task boundary (NimBLE host task -> main loop task), so implementations must
// only do cheap, thread-safe work here (e.g. copy into a lock-guarded buffer
// and set a flag CompanionModeActivity::loop() polls), per the ISR/task
// shared-state rules in root CLAUDE.md. `field` is 0x01 for title, 0x02 for
// body, 0x03 for content-id (kFinalFieldFlag already stripped), matching the
// protocol doc. `data`/`len` are only valid for the duration of the call.
// `final` mirrors kFinalFieldFlag from this field's START packet — callers
// that defer applying updates until a batch is complete (see
// CompanionModeActivity::onContentField()) use this to know when to commit.
using ContentFieldCallback = void (*)(uint8_t field, const uint8_t* data, size_t len, bool final);
void setContentFieldCallback(ContentFieldCallback cb);

// Callback for a Status characteristic write (phone -> device), e.g. a
// READ_LATER_SAVED acknowledgement after a READ_LATER button notify. Same
// task-boundary rules as ContentFieldCallback: invoked from the NimBLE host
// task, must only do cheap thread-safe work (set a flag/value under a lock
// that loop() polls).
using StatusCallback = void (*)(uint8_t status);
void setStatusCallback(StatusCallback cb);

}  // namespace companionble
