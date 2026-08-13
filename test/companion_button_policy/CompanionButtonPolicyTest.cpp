#include <gtest/gtest.h>

#include "CompanionButtonPolicy.h"

namespace {

using companionble::ButtonRouting;
using companionbuttons::decide;

TEST(CompanionButtonPolicy, NoneIsInertRegardlessOfConnection) {
  for (const bool peerConnected : {false, true}) {
    const auto d = decide(ButtonRouting::None, peerConnected);
    EXPECT_EQ(d.action, ButtonRouting::None);
    EXPECT_FALSE(d.notify);
    EXPECT_FALSE(d.showHint);
  }
}

// The key regression test: this must fail before CompanionButtonPolicy
// exists (there is no decide() to call), and must still pass once it does --
// a Remote-routed button must not show its hint (or notify) once the peer
// has disconnected. Before this fix, labelFor() only blanked the hint for
// ButtonRouting::None, so a Remote button kept drawing its app-supplied
// label after disconnect and silently did nothing when pressed.
TEST(CompanionButtonPolicy, RemoteHidesHintWhenPeerDisconnected) {
  const auto d = decide(ButtonRouting::Remote, /*peerConnected=*/false);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_FALSE(d.notify);
  EXPECT_FALSE(d.showHint);
}

TEST(CompanionButtonPolicy, RemoteShowsHintAndNotifiesWhenPeerConnected) {
  const auto d = decide(ButtonRouting::Remote, /*peerConnected=*/true);
  EXPECT_EQ(d.action, ButtonRouting::None);
  EXPECT_TRUE(d.notify);
  EXPECT_TRUE(d.showHint);
}

TEST(CompanionButtonPolicy, LocalRoutingsRunLocallyRegardlessOfConnection) {
  const ButtonRouting kLocalRoutings[] = {
      ButtonRouting::LocalPagePrev,        ButtonRouting::LocalPageNext,        ButtonRouting::LocalSleep,
      ButtonRouting::LocalListMoveUp,      ButtonRouting::LocalListMoveDown,    ButtonRouting::LocalListSwitchLeft,
      ButtonRouting::LocalListSwitchRight, ButtonRouting::LocalListToggleCheck, ButtonRouting::LocalListBack,
  };

  for (const ButtonRouting routing : kLocalRoutings) {
    for (const bool peerConnected : {false, true}) {
      const auto d = decide(routing, peerConnected);
      EXPECT_EQ(d.action, routing) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
      EXPECT_FALSE(d.notify) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
      EXPECT_TRUE(d.showHint) << "routing=" << static_cast<int>(routing) << " peerConnected=" << peerConnected;
    }
  }
}

}  // namespace
