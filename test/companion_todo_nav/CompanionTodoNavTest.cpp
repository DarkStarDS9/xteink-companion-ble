#include "CompanionTodoNav.h"

#include <gtest/gtest.h>

namespace {

using companiontodo::Nav;

TEST(CompanionTodoNav, FreshStateIsEmptyAndInert) {
  Nav nav;
  EXPECT_TRUE(nav.empty());
  EXPECT_EQ(nav.listIndex(), 0u);
  EXPECT_EQ(nav.listCount(), 0u);
  EXPECT_EQ(nav.cursor(), 0u);
  EXPECT_EQ(nav.windowStart(), 0u);
  EXPECT_FALSE(nav.moveUp());
  EXPECT_FALSE(nav.moveDown());
  EXPECT_FALSE(nav.switchListLeft());
  EXPECT_FALSE(nav.switchListRight());
}

TEST(CompanionTodoNav, ZeroListDocumentStaysEmpty) {
  Nav nav;
  nav.setListCount(0);
  nav.setCurrentList(0);
  EXPECT_TRUE(nav.empty());
  EXPECT_FALSE(nav.moveDown());
  EXPECT_FALSE(nav.switchListRight());
}

TEST(CompanionTodoNav, SingleListZeroItemsIsEmptyButNotNoList) {
  Nav nav;
  nav.setListCount(1);
  nav.setCurrentList(0);
  EXPECT_EQ(nav.listCount(), 1u);
  EXPECT_TRUE(nav.empty());  // itemCount == 0
  EXPECT_FALSE(nav.moveUp());
  EXPECT_FALSE(nav.moveDown());
  // One list: nothing to switch to either.
  EXPECT_FALSE(nav.switchListLeft());
  EXPECT_FALSE(nav.switchListRight());
}

TEST(CompanionTodoNav, CursorMovesAndClampsAtEnds) {
  Nav nav;
  nav.setListCount(1);
  nav.setVisibleCapacity(10);  // whole list fits; no paging to interfere
  nav.setCurrentList(3);
  EXPECT_FALSE(nav.empty());
  EXPECT_EQ(nav.cursor(), 0u);
  EXPECT_FALSE(nav.moveUp()) << "already at the first item";

  EXPECT_TRUE(nav.moveDown());
  EXPECT_EQ(nav.cursor(), 1u);
  EXPECT_TRUE(nav.moveDown());
  EXPECT_EQ(nav.cursor(), 2u);
  EXPECT_FALSE(nav.moveDown()) << "already at the last item (index 2 of 3)";
  EXPECT_EQ(nav.cursor(), 2u);

  EXPECT_TRUE(nav.moveUp());
  EXPECT_EQ(nav.cursor(), 1u);
}

TEST(CompanionTodoNav, WindowPaginatesWhenCursorRunsOffScreen) {
  Nav nav;
  nav.setListCount(1);
  nav.setVisibleCapacity(3);
  nav.setCurrentList(10);
  EXPECT_EQ(nav.windowStart(), 0u);

  // Move to item 3 (0-indexed) -- the 4th item, first one off the initial
  // [0,3) window -- window must slide so it's the last row shown.
  for (int i = 0; i < 3; ++i) nav.moveDown();
  EXPECT_EQ(nav.cursor(), 3u);
  EXPECT_EQ(nav.windowStart(), 1u) << "window should have slid by exactly one row";

  for (int i = 0; i < 6; ++i) nav.moveDown();  // walk to the last item (index 9)
  EXPECT_EQ(nav.cursor(), 9u);
  EXPECT_EQ(nav.windowStart(), 7u) << "last full page: [7,10)";

  // And back up to the very first item should re-anchor the window at 0.
  for (int i = 0; i < 9; ++i) nav.moveUp();
  EXPECT_EQ(nav.cursor(), 0u);
  EXPECT_EQ(nav.windowStart(), 0u);
}

TEST(CompanionTodoNav, ShrinkingVisibleCapacityReclampsWindow) {
  Nav nav;
  nav.setListCount(1);
  nav.setVisibleCapacity(10);
  nav.setCurrentList(10);
  for (int i = 0; i < 9; ++i) nav.moveDown();
  EXPECT_EQ(nav.cursor(), 9u);
  EXPECT_EQ(nav.windowStart(), 0u);  // whole list fit before

  nav.setVisibleCapacity(4);  // e.g. an orientation change shrinks the viewport
  EXPECT_EQ(nav.windowStart(), 6u) << "window must slide to keep the cursor visible";
}

TEST(CompanionTodoNav, SwitchListWrapsBothDirections) {
  Nav nav;
  nav.setListCount(3);
  EXPECT_EQ(nav.listIndex(), 0u);

  EXPECT_TRUE(nav.switchListLeft());
  EXPECT_EQ(nav.listIndex(), 2u) << "left from the first list wraps to the last";

  EXPECT_TRUE(nav.switchListRight());
  EXPECT_EQ(nav.listIndex(), 0u);

  EXPECT_TRUE(nav.switchListRight());
  EXPECT_EQ(nav.listIndex(), 1u);
}

TEST(CompanionTodoNav, SwitchListResetsCursorAndWindow) {
  Nav nav;
  nav.setListCount(2);
  nav.setVisibleCapacity(3);
  nav.setCurrentList(10);
  for (int i = 0; i < 5; ++i) nav.moveDown();
  EXPECT_GT(nav.cursor(), 0u);
  EXPECT_GT(nav.windowStart(), 0u);

  ASSERT_TRUE(nav.switchListRight());
  EXPECT_EQ(nav.listIndex(), 1u);
  EXPECT_EQ(nav.cursor(), 0u);
  EXPECT_EQ(nav.windowStart(), 0u);
}

TEST(CompanionTodoNav, SwitchingToListWithFewerItemsThanStrayCursorIsSafe) {
  // Exercises the caller contract directly: switchListRight() always resets
  // cursor/window to 0 itself, so a stray old cursor value can never outlive
  // the switch -- but setCurrentList() must also cope on its own if some
  // future caller ever skips the switch* reset (e.g. a document reload that
  // shrinks the *current* list out from under an unmoved cursor).
  Nav nav;
  nav.setListCount(1);
  nav.setVisibleCapacity(5);
  nav.setCurrentList(20);
  for (int i = 0; i < 15; ++i) nav.moveDown();
  EXPECT_EQ(nav.cursor(), 15u);

  // Document reloaded; same list index, but now far fewer items (e.g. the
  // phone re-pushed a shorter list).
  nav.setCurrentList(3);
  EXPECT_EQ(nav.cursor(), 2u) << "clamped to the new last item";
  EXPECT_LE(nav.windowStart() + nav.visibleCapacity(), 3u + nav.visibleCapacity());
  EXPECT_EQ(nav.windowStart(), 0u);
}

TEST(CompanionTodoNav, ShrinkingListCountClampsCurrentIndex) {
  Nav nav;
  nav.setListCount(5);
  ASSERT_TRUE(nav.switchListLeft());
  ASSERT_TRUE(nav.switchListLeft());
  EXPECT_EQ(nav.listIndex(), 3u);

  // Document reloaded with only 2 lists now.
  nav.setListCount(2);
  EXPECT_EQ(nav.listIndex(), 1u) << "clamped to the new last valid index";
}

TEST(CompanionTodoNav, ResetClearsDocumentStateButNotViewportCapacity) {
  Nav nav;
  nav.setVisibleCapacity(7);
  nav.setListCount(2);
  nav.setCurrentList(5);
  nav.moveDown();

  nav.reset();
  EXPECT_TRUE(nav.empty());
  EXPECT_EQ(nav.listCount(), 0u);
  EXPECT_EQ(nav.cursor(), 0u);
  EXPECT_EQ(nav.windowStart(), 0u);
  EXPECT_EQ(nav.visibleCapacity(), 7u) << "a viewport property, not document state";
}

}  // namespace
