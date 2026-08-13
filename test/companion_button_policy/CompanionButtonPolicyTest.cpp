#include <gtest/gtest.h>

#include "CompanionButtonPolicy.h"

namespace {

using companionble::ButtonRouting;
using companionbuttons::decide;

TEST(CompanionButtonPolicy, NoneIsInertRegardlessOfConnection) {
  for (const bool peerConnected : {false, true}) {
    const auto d = decide(/*flags=*/0x00, ButtonRouting::None, peerConnected);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_FALSE(d.notify);
    EXPECT_FALSE(d.showHint);
  }
}

// Flags are meaningless without a routing to modify: None ignores them
// entirely, in every combination and connection state.
TEST(CompanionButtonPolicy, FlagsAreInertOnNone) {
  const uint8_t kAllFlagCombos[] = {
      companionble::kButtonFlagAlsoNotify,
      companionble::kButtonFlagLocalOnlyOffline,
      static_cast<uint8_t>(companionble::kButtonFlagAlsoNotify | companionble::kButtonFlagLocalOnlyOffline),
  };
  for (const uint8_t flags : kAllFlagCombos) {
    for (const bool peerConnected : {false, true}) {
      const auto d = decide(flags, ButtonRouting::None, peerConnected);
      EXPECT_EQ(d.action, ButtonRouting::None);
      EXPECT_FALSE(d.notify);
      EXPECT_FALSE(d.showHint);
    }
  }
}

// The key regression test: this must fail before CompanionButtonPolicy
// exists (there is no decide() to call), and must still pass once it does --
// a Remote-routed button must not show its hint (or notify) once the peer
// has disconnected. Before this fix, labelFor() only blanked the hint for
// ButtonRouting::None, so a Remote button kept drawing its app-supplied
// label after disconnect and silently did nothing when pressed.
TEST(CompanionButtonPolicy, RemoteHidesHintWhenPeerDisconnected) {
  const auto d = decide(/*flags=*/0x00, ButtonRouting::Remote, /*peerConnected=*/false);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_FALSE(d.notify);
  EXPECT_FALSE(d.showHint);
}

TEST(CompanionButtonPolicy, RemoteShowsHintAndNotifiesWhenPeerConnected) {
  const auto d = decide(/*flags=*/0x00, ButtonRouting::Remote, /*peerConnected=*/true);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_TRUE(d.notify);
  EXPECT_TRUE(d.showHint);
}

// ALSO_NOTIFY is idempotent on Remote -- it already notifies when connected
// and does nothing when disconnected, flag or no flag.
TEST(CompanionButtonPolicy, RemoteWithAlsoNotifyFlagUnchanged) {
  {
    const auto d = decide(companionble::kButtonFlagAlsoNotify, ButtonRouting::Remote, /*peerConnected=*/false);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_FALSE(d.notify);
    EXPECT_FALSE(d.showHint);
  }
  {
    const auto d = decide(companionble::kButtonFlagAlsoNotify, ButtonRouting::Remote, /*peerConnected=*/true);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_TRUE(d.notify);
    EXPECT_TRUE(d.showHint);
  }
}

// LOCAL_ONLY_OFFLINE is meaningless on Remote -- there is no local action to
// suppress -- so it must not change anything either.
TEST(CompanionButtonPolicy, RemoteWithLocalOnlyOfflineFlagUnchanged) {
  {
    const auto d = decide(companionble::kButtonFlagLocalOnlyOffline, ButtonRouting::Remote, /*peerConnected=*/false);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_FALSE(d.notify);
    EXPECT_FALSE(d.showHint);
  }
  {
    const auto d = decide(companionble::kButtonFlagLocalOnlyOffline, ButtonRouting::Remote, /*peerConnected=*/true);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_TRUE(d.notify);
    EXPECT_TRUE(d.showHint);
  }
}

TEST(CompanionButtonPolicy, LocalRoutingsRunLocallyRegardlessOfConnection) {
  const ButtonRouting kLocalRoutings[] = {
      ButtonRouting::LocalPagePrev,        ButtonRouting::LocalPageNext,        ButtonRouting::LocalSleep,
      ButtonRouting::LocalListMoveUp,      ButtonRouting::LocalListMoveDown,    ButtonRouting::LocalListSwitchLeft,
      ButtonRouting::LocalListSwitchRight, ButtonRouting::LocalListToggleCheck, ButtonRouting::LocalBack,
      ButtonRouting::LocalGalleryPrev,     ButtonRouting::LocalGalleryNext,
  };

  for (const ButtonRouting routing : kLocalRoutings) {
    for (const bool peerConnected : {false, true}) {
      const auto d = decide(/*flags=*/0x00, routing, peerConnected);
      EXPECT_EQ(d.action, routing) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
      EXPECT_FALSE(d.notify) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
      EXPECT_TRUE(d.showHint) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
    }
  }
}

// The full ALSO_NOTIFY / LOCAL_ONLY_OFFLINE behaviour table from the design,
// for every local routing (a plain local action, all six LocalList*/LocalBack
// values, and the two LocalGallery* values) in both connection states. See
// the table in docs/companion-multi-app-design.md §7.
class CompanionButtonPolicyFlagsTest : public ::testing::TestWithParam<ButtonRouting> {};

TEST_P(CompanionButtonPolicyFlagsTest, PlainLocalRunsRegardlessOfConnection) {
  const ButtonRouting routing = GetParam();
  for (const bool peerConnected : {false, true}) {
    const auto d = decide(/*flags=*/0x00, routing, peerConnected);
    EXPECT_EQ(d.action, routing);
    EXPECT_FALSE(d.notify);
    EXPECT_TRUE(d.showHint);
  }
}

TEST_P(CompanionButtonPolicyFlagsTest, AlsoNotifyRunsLocalAndNotifiesWhenConnected) {
  const ButtonRouting routing = GetParam();
  const auto d = decide(companionble::kButtonFlagAlsoNotify, routing, /*peerConnected=*/true);
  EXPECT_EQ(d.action, routing);
  EXPECT_TRUE(d.notify);
  EXPECT_TRUE(d.showHint);
}

TEST_P(CompanionButtonPolicyFlagsTest, AlsoNotifyRunsLocalOnlyWhenOffline) {
  const ButtonRouting routing = GetParam();
  const auto d = decide(companionble::kButtonFlagAlsoNotify, routing, /*peerConnected=*/false);
  EXPECT_EQ(d.action, routing);
  EXPECT_FALSE(d.notify);
  EXPECT_TRUE(d.showHint);
}

TEST_P(CompanionButtonPolicyFlagsTest, LocalOnlyOfflineSuppressesLocalAndHidesHintWhenConnected) {
  const ButtonRouting routing = GetParam();
  const auto d = decide(companionble::kButtonFlagLocalOnlyOffline, routing, /*peerConnected=*/true);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_FALSE(d.notify);
  EXPECT_FALSE(d.showHint);
}

TEST_P(CompanionButtonPolicyFlagsTest, LocalOnlyOfflineRunsLocalWhenOffline) {
  const ButtonRouting routing = GetParam();
  const auto d = decide(companionble::kButtonFlagLocalOnlyOffline, routing, /*peerConnected=*/false);
  EXPECT_EQ(d.action, routing);
  EXPECT_FALSE(d.notify);
  EXPECT_TRUE(d.showHint);
}

TEST_P(CompanionButtonPolicyFlagsTest, BothFlagsNotifyOnlyWhenConnected) {
  const ButtonRouting routing = GetParam();
  const uint8_t flags = companionble::kButtonFlagAlsoNotify | companionble::kButtonFlagLocalOnlyOffline;
  const auto d = decide(flags, routing, /*peerConnected=*/true);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_TRUE(d.notify);
  EXPECT_TRUE(d.showHint);
}

TEST_P(CompanionButtonPolicyFlagsTest, BothFlagsRunLocalWhenOffline) {
  const ButtonRouting routing = GetParam();
  const uint8_t flags = companionble::kButtonFlagAlsoNotify | companionble::kButtonFlagLocalOnlyOffline;
  const auto d = decide(flags, routing, /*peerConnected=*/false);
  EXPECT_EQ(d.action, routing);
  EXPECT_FALSE(d.notify);
  EXPECT_TRUE(d.showHint);
}

// Reserved bits 0x30 must be ignored, not rejected: a declaration that sets
// them behaves identically to one that doesn't, for every flag/connection
// combination above.
TEST_P(CompanionButtonPolicyFlagsTest, ReservedBitsAreIgnored) {
  const ButtonRouting routing = GetParam();
  const uint8_t kReservedBits = 0x30;
  for (const uint8_t baseFlags :
       {static_cast<uint8_t>(0x00), companionble::kButtonFlagAlsoNotify, companionble::kButtonFlagLocalOnlyOffline,
        static_cast<uint8_t>(companionble::kButtonFlagAlsoNotify | companionble::kButtonFlagLocalOnlyOffline)}) {
    for (const bool peerConnected : {false, true}) {
      const auto withoutReserved = decide(baseFlags, routing, peerConnected);
      const auto withReserved = decide(static_cast<uint8_t>(baseFlags | kReservedBits), routing, peerConnected);
      EXPECT_EQ(withoutReserved.action, withReserved.action);
      EXPECT_EQ(withoutReserved.notify, withReserved.notify);
      EXPECT_EQ(withoutReserved.showHint, withReserved.showHint);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(AllLocalRoutings, CompanionButtonPolicyFlagsTest,
                         ::testing::Values(ButtonRouting::LocalPagePrev, ButtonRouting::LocalPageNext,
                                           ButtonRouting::LocalSleep, ButtonRouting::LocalListMoveUp,
                                           ButtonRouting::LocalListMoveDown, ButtonRouting::LocalListSwitchLeft,
                                           ButtonRouting::LocalListSwitchRight, ButtonRouting::LocalListToggleCheck,
                                           ButtonRouting::LocalBack, ButtonRouting::LocalGalleryPrev,
                                           ButtonRouting::LocalGalleryNext));

// companionbuttons::anyBound() -- whether a peer declared ANY binding at all
// across a whole button map. Backs the firmware-owned Back fallback in
// CompanionModeActivity::handleListNav()'s default: arm and in
// handlePickerInput()'s browsed-gallery Back case: either may only fire
// when this is false for the peer's entire map, not just the Back button.

TEST(CompanionButtonPolicyAnyBound, EmptyMapIsUnbound) {
  const ButtonRouting routings[] = {ButtonRouting::None, ButtonRouting::None, ButtonRouting::None, ButtonRouting::None,
                                    ButtonRouting::None, ButtonRouting::None, ButtonRouting::None};
  EXPECT_FALSE(companionbuttons::anyBound(routings, 7));
}

TEST(CompanionButtonPolicyAnyBound, ZeroCountIsUnbound) {
  const ButtonRouting routings[] = {ButtonRouting::Remote};
  EXPECT_FALSE(companionbuttons::anyBound(routings, 0));
}

// The must-be-false-only-for-the-whole-map requirement: a peer that bound
// exactly one button, anywhere in the map, is bound -- not just a peer that
// bound Back specifically.
TEST(CompanionButtonPolicyAnyBound, OneUnrelatedButtonBoundIsBound) {
  const ButtonRouting routings[] = {ButtonRouting::None, ButtonRouting::None, ButtonRouting::Remote,
                                    ButtonRouting::None, ButtonRouting::None, ButtonRouting::None,
                                    ButtonRouting::None};
  EXPECT_TRUE(companionbuttons::anyBound(routings, 7));
}

TEST(CompanionButtonPolicyAnyBound, LastEntryBoundIsBound) {
  const ButtonRouting routings[] = {ButtonRouting::None,     ButtonRouting::None, ButtonRouting::None,
                                    ButtonRouting::None,     ButtonRouting::None, ButtonRouting::None,
                                    ButtonRouting::LocalBack};
  EXPECT_TRUE(companionbuttons::anyBound(routings, 7));
}

TEST(CompanionButtonPolicyAnyBound, EveryEntryBoundIsBound) {
  const ButtonRouting routings[] = {ButtonRouting::LocalListMoveUp,
                                    ButtonRouting::LocalListMoveDown,
                                    ButtonRouting::LocalListSwitchLeft,
                                    ButtonRouting::LocalListSwitchRight,
                                    ButtonRouting::LocalListToggleCheck,
                                    ButtonRouting::LocalBack,
                                    ButtonRouting::Remote};
  EXPECT_TRUE(companionbuttons::anyBound(routings, 7));
}

// companionbuttons::isGalleryNavAction() -- the "which decide() outcomes are
// mine" rule CompanionModeActivity::handleGalleryNav() claims a press on. The
// trap this pins: unlike Screen::List (which claims all six of its buttons
// unconditionally), Screen::Image must NOT claim a press that resolves to
// anything but LocalGalleryPrev/LocalGalleryNext -- most importantly None
// (unbound) and Remote (e.g. Snap2Ink's camera shutter on Up/Down), both of
// which must fall through to handleMappedButton() instead of being eaten
// here. See handleGalleryNav()'s own .cpp comment for the caller-side half of
// this (host-unreachable: CompanionModeActivity.cpp is not host-buildable).

TEST(CompanionButtonPolicyIsGalleryNavAction, GalleryPrevIsGalleryNavAction) {
  EXPECT_TRUE(companionbuttons::isGalleryNavAction(ButtonRouting::LocalGalleryPrev));
}

TEST(CompanionButtonPolicyIsGalleryNavAction, GalleryNextIsGalleryNavAction) {
  EXPECT_TRUE(companionbuttons::isGalleryNavAction(ButtonRouting::LocalGalleryNext));
}

// Every other decide()-result outcome must NOT be claimed -- in particular
// None (the outcome for an unbound button) and Remote's local action (always
// None, per decide()'s own contract), so the shutter/unbound cases fall
// through to handleMappedButton() on Screen::Image.
TEST(CompanionButtonPolicyIsGalleryNavAction, EveryOtherRoutingIsNotGalleryNavAction) {
  const ButtonRouting kNotGalleryNav[] = {
      ButtonRouting::None,
      ButtonRouting::Remote,
      ButtonRouting::LocalPagePrev,
      ButtonRouting::LocalPageNext,
      ButtonRouting::LocalSleep,
      ButtonRouting::LocalListMoveUp,
      ButtonRouting::LocalListMoveDown,
      ButtonRouting::LocalListSwitchLeft,
      ButtonRouting::LocalListSwitchRight,
      ButtonRouting::LocalListToggleCheck,
      ButtonRouting::LocalBack,
  };
  for (const ButtonRouting routing : kNotGalleryNav) {
    EXPECT_FALSE(companionbuttons::isGalleryNavAction(routing)) << "routing=" << static_cast<int>(routing);
  }
}

// The end-to-end shape of the trap, expressed purely through decide(): an
// unbound Up/Down (routing None -- what routingFor() returns for a button
// the peer never declared, including a browsed-gallery `buttons` map that
// was never loaded at all, i.e. trap #2) decides to action None, which
// isGalleryNavAction() correctly rejects -- so handleGalleryNav() must return
// false and defer to handleMappedButton(), regardless of connection state.
TEST(CompanionButtonPolicyIsGalleryNavAction, UnboundButtonFallsThroughRegardlessOfConnection) {
  for (const bool peerConnected : {false, true}) {
    const auto decision = decide(/*flags=*/0x00, ButtonRouting::None, peerConnected);
    EXPECT_FALSE(companionbuttons::isGalleryNavAction(decision.action));
  }
}

// Same shape for a peer that routed Up/Down to Remote instead (Snap2Ink's
// camera shutter): decide() never returns Remote as the resolved action (see
// RemoteHidesHintWhenPeerDisconnected/RemoteShowsHintAndNotifiesWhenPeerConnected
// above), so isGalleryNavAction() rejects it here too -- the shutter press
// must reach handleMappedButton(), not be silently eaten as gallery paging.
TEST(CompanionButtonPolicyIsGalleryNavAction, RemoteRoutedButtonFallsThroughRegardlessOfConnection) {
  for (const bool peerConnected : {false, true}) {
    const auto decision = decide(/*flags=*/0x00, ButtonRouting::Remote, peerConnected);
    EXPECT_FALSE(companionbuttons::isGalleryNavAction(decision.action));
  }
}

}  // namespace
