#pragma once

#include <cstdint>

#include "CompanionBle.h"

// The "what does this button do right now" decision, extracted out of
// CompanionModeActivity.cpp's handleMappedButton()/labelFor()/listLabelFor()
// so it can be covered by a host gtest suite instead of only hardware
// regressions (see test/companion_button_policy/), exactly as
// CompanionBatchModel, CompanionConnPolicy and the UI-declaration codec were
// extracted before it. CompanionModeActivity.cpp pulls in Arduino, NimBLE and
// the HAL and therefore cannot be host-built at all; nothing in here may
// acquire a dependency that changes that. <cstdint> and CompanionBle.h's
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
  // labelFor()/listLabelFor() use to decide whether to draw the button's
  // hint at all.
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

}  // namespace companionbuttons
