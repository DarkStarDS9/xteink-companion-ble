#pragma once

#include <cstddef>
#include <cstdint>

#include "CompanionBle.h"

// The "what does this button do right now" decision, extracted out of
// CompanionModeActivity.cpp's handleMappedButton()/labelFor() so it can be
// covered by a host gtest suite instead of only hardware regressions (see
// test/companion_button_policy/), exactly as CompanionBatchModel,
// CompanionConnPolicy and the UI-declaration codec were extracted before it.
// CompanionModeActivity.cpp pulls in Arduino, NimBLE and the HAL and
// therefore cannot be host-built at all; nothing in here may acquire a
// dependency that changes that. <cstddef>, <cstdint> and CompanionBle.h's
// enums, and nothing else.
//
// Stateless: one call in, one struct out, no clock, no allocation. The
// caller still owns everything position-dependent -- hiding LocalPagePrev at
// page 0, LocalPageNext on the last page, or the list switch buttons when
// there is only one document -- because that is about *where on screen* the
// button would land, not about what the button is routed to.
namespace companionbuttons {

// What a single button press resolves to right now.
struct ButtonDecision {
  // The local action to run, or None if there isn't one. Equal to the
  // routing that was asked about, except Remote always decides None here --
  // relaying a press to the app is the caller's job (notifyHeldButton()),
  // not a "local action".
  companionble::ButtonRouting action;
  // Whether a button event should be sent to the app over the wire.
  bool notify;
  // Whether pressing this button would do anything right now -- the signal
  // labelFor() uses to decide whether to draw the button's hint at all.
  bool showHint;
};

// Decides what `routing` resolves to given `flags` (the high nibble of the
// button-map entry's byte 0 -- kButtonFlagAlsoNotify / kButtonFlagLocalOnly-
// Offline, see CompanionBle.h; reserved bits 0x30 are ignored, not
// validated -- that is the caller's job, this function tolerates them) and
// whether a peer currently holds the foreground link
// (CompanionModeActivity's `!foregroundPeerKey.empty()` test, not the raw
// `connected` member -- see the call sites this replaces).
//
// flags are inert on ButtonRouting::None and ::Remote: there is no local
// action for them to modify. See docs/companion-multi-app-design.md §7 for
// the full behaviour table.
ButtonDecision decide(uint8_t flags, companionble::ButtonRouting routing, bool peerConnected);

// Whether any entry of a button map (one ButtonRouting per physical button,
// `count` of them) is bound to something other than ButtonRouting::None --
// i.e. whether the peer declared ANY control-scheme binding at all, across
// the whole map, not just a subset of buttons.
//
// Used to decide whether firmware may supply its own fallback out of a
// screen whose navigation is otherwise fully app-declared with no default
// (Screen::List's Back, see CompanionModeActivity::handleListNav()): with
// bindings fully app-declared and no defaults, a peer that binds no Back at
// all leaves the screen with no way out but the power button, and -- during
// an offline browse, where BLE never starts -- no app around to push a
// corrected map. Firmware may only step in when the peer bound NOTHING,
// i.e. this returns false for the whole map; a peer that bound exactly one
// unrelated button is still taken at face value, absence of Back included.
bool anyBound(const companionble::ButtonRouting* routings, size_t count);

// Whether `action` (a decide() result's `.action` field) is one of the two
// gallery-navigation routings -- LocalGalleryPrev/LocalGalleryNext.
//
// Unlike Screen::List (handleListNav(), which claims all six of its buttons
// unconditionally once a LIST peer's exclusive shape makes that safe --
// docs/companion-todo-list-design.md §5), Screen::Image does NOT claim every
// press it sees: an unbound Up/Down (decide() resolves to None) or one routed
// to something else entirely -- most importantly Remote, for an app like
// Snap2Ink using Up/Down as a camera shutter -- must fall through to
// handleMappedButton() instead of being silently eaten by gallery navigation.
// handleGalleryNav() calls this on decide()'s result to decide whether IT is
// the one that gets to act, rather than inlining the two-way comparison
// itself, so the "which actions are mine" rule is covered here instead of
// only by reading CompanionModeActivity.cpp (not host-buildable -- see this
// header's own doc comment).
bool isGalleryNavAction(companionble::ButtonRouting action);

}  // namespace companionbuttons
