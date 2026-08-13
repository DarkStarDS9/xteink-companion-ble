#pragma once

// Serial remote control for automated on-target testing of Companion Mode.
//
// WHY THIS EXISTS: v6 enrollment requires a physical CONFIRM press on the device
// to accept an unknown peer. A BLE script driving the handshake from a dev
// machine therefore cannot complete a pairing on its own, which would leave the
// single most important new path in v6 manually tested forever. Injecting button
// events over serial closes that loop, and lets a host drive both halves of a
// test — BLE on one transport, buttons on the other.
//
// DELIBERATELY THIN. This is a debug view over state that already exists, not a
// second control plane. Nothing here may do something a phone cannot do over
// BLE; if it could, the tested path would stop being the shipped path and the
// tests would be worthless. Injection happens at the activity's button-routing
// seam, so a test exercises the real button map, the real hold-tick logic and
// the real notify path — everything except GPIO debounce, which is not what any
// of this is testing.
//
// Compiled out entirely unless COMPANION_TEST_CONSOLE is defined; see the
// [env:test] target in platformio.ini. A normal build contains none of it.
//
// MEMORY: one virtual-press struct (~16 bytes) and a screen-name function
// pointer. No buffers — command lines are read by main.cpp's existing `CMD:`
// dispatcher, whose String this only borrows. Well under 100 bytes, and only in
// a build that is never shipped.
//
// Command set is documented in docs/companion-test-console.md.

#ifdef COMPANION_TEST_CONSOLE

#include <Arduino.h>

#include "CompanionBle.h"

namespace companiontest {

// Handles one `CMD:`-prefixed line (with the prefix already stripped). Returns
// true if the command was one of ours. Called from main.cpp's dispatcher.
bool handleCommand(const String& command);

// Advances any virtual button hold. Called once per main-loop iteration.
void update();

// --- Injection, read by CompanionModeActivity ------------------------------
// These mirror the MappedInputManager calls the activity already makes, so the
// activity ORs them in at one seam per call rather than growing a second input
// path.

// True once per injected press, then consumed — matches wasPressed() semantics.
bool wasPressed(companionble::ButtonId button);
// True while an injected hold is in progress for this button.
bool isPressed(companionble::ButtonId button);
// True once when an injected hold ends — matches wasReleased() semantics.
bool wasReleased(companionble::ButtonId button);
// Elapsed time of the in-progress injected hold, in ms.
unsigned long heldTimeMs();
// True while any injected press/hold is live, so the activity knows to consult
// the injected timings rather than the hardware's.
bool holdInProgress();

// --- Read-back -------------------------------------------------------------

// The activity registers a callback returning its current screen name, so
// CSTATE can report it without this module knowing anything about screens.
using ScreenNameProvider = const char* (*)();
void setScreenNameProvider(ScreenNameProvider provider);

// One declared tag, as reported by CTAGS.
struct TagReport {
  uint8_t id;
  uint8_t state;
  char label[companionble::kMaxTagLabelLen + 1];
};

// Reports the foreground peer's live tag state. This is the one piece of
// protocol state a BLE client genuinely cannot confirm for itself: the Status
// characteristic is write-without-response, so nothing tells an app whether its
// tag write landed. Reading it back over serial is how a test asserts it did.
using TagStateProvider = uint8_t (*)(TagReport* out, uint8_t maxTags);
void setTagStateProvider(TagStateProvider provider);

// Snapshot of companiontodo::Nav's cursor/paging state for CLISTNAV. Filled
// only when Screen::List is up; see CompanionModeActivity::reportListNav().
struct ListNavReport {
  bool onListScreen = false;
  uint16_t listIndex = 0;
  uint16_t listCount = 0;
  uint16_t cursor = 0;
  uint16_t windowStart = 0;
  uint16_t itemCount = 0;
};

// Reports companiontodo::Nav's live position. Verifying "the cursor moved" or
// "the list switched" used to mean diffing a 52 KB screenshot -- fragile, and
// not exclusive against the device's own serial logging (see
// docs/companion-test-console.md's SCREENSHOT section). This exposes the same
// numbers render() already reads, with none of that.
using ListNavStateProvider = bool (*)(ListNavReport* out);
void setListNavStateProvider(ListNavStateProvider provider);

}  // namespace companiontest

#endif  // COMPANION_TEST_CONSOLE
