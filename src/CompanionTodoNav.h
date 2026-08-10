#pragma once

#include <cstdint>

// The Screen::List cursor/paging/list-switching state machine, extracted out
// of CompanionModeActivity.cpp so it can be covered by a host gtest suite
// (test/companion_todo_nav/) instead of only hardware regressions -- see
// CompanionConnPolicy.h and CompanionBatchModel.h for the same rationale
// applied to the two other areas of this file that have regressed the most.
// Off-by-ones in cursor/window arithmetic are exactly the class of bug that
// slips through on-device eyeballing and is trivial to pin down with a table
// of inputs here.
//
// Deliberately knows NOTHING about the document itself -- no strings, no
// group/item structure, no SD, no ArduinoJson. It is fed plain counts
// (how many lists, how many items in the current list, how many rows fit on
// screen) by CompanionModeActivity, which gets those counts from a
// companiontodo::Visitor walk of companionpeer::loadListDocument(). That
// walk is the expensive, view-changes-only operation
// (docs/companion-todo-list-design.md §8's ~49 KB transient JsonDocument);
// this class is the cheap, always-in-RAM state it's walked to refresh.
//
// MEMORY: five uint16_t-ish fields, comfortably inside the ~200 B design §8
// budgets for list/cursor nav state (§11 of the multi-app design's gallery
// nav state, which this mirrors).
namespace companiontodo {

class Nav {
 public:
  // Resets to "no document": zero lists, zero items, cursor and window at 0.
  // Call on foreground handover away from a LIST peer, or when a peer's
  // document fails to load at all.
  void reset();

  // Records how many lists the just-reloaded document has, and clamps
  // listIndex() into range. Call this BEFORE setCurrentList() on every
  // reload -- setCurrentList()'s itemCount argument is only meaningful for
  // whichever list index this call leaves current. listCount == 0 is
  // equivalent to reset().
  void setListCount(uint16_t listCount);

  // Records the item count of the list at listIndex() (the caller re-walks
  // the document targeting that index to get this number -- see
  // CompanionModeActivity::reloadListView()) and clamps cursor()/
  // windowStart() into range. Call after every setListCount() and after
  // setVisibleCapacity() changes.
  void setCurrentList(uint16_t itemCount);

  // How many item rows (not counting group-header rows) the screen can show
  // at once. Comes from the renderer's viewport calc, same idea as
  // CompanionModeActivity::linesPerPage. 0 is clamped to 1 so window math
  // never divides by zero.
  void setVisibleCapacity(uint8_t capacity);

  // Moves the cursor by one item, clamped at the first/last item (no
  // wraparound -- Up/Down inside one list is not the same gesture as
  // switching lists). Paginates windowStart() if the move carries the
  // cursor off the visible window. Returns true if the cursor actually
  // moved (false at an end-of-list clamp, or on an empty list), so the
  // caller can skip a redraw for a press that did nothing.
  bool moveUp();
  bool moveDown();

  // Switches to the previous/next list, wrapping at either end (mirrors
  // handleGalleryNav()'s Up/Down wraparound). Resets cursor and window to 0
  // on the new list -- the caller must follow with setCurrentList() once it
  // has re-walked the document for the new list's item count. Returns false
  // (no-op) when there is nothing to switch to (0 or 1 lists).
  bool switchListLeft();
  bool switchListRight();

  uint16_t listIndex() const { return listIndex_; }
  uint16_t listCount() const { return listCount_; }
  uint16_t cursor() const { return cursor_; }
  uint16_t windowStart() const { return windowStart_; }
  uint16_t itemCount() const { return itemCount_; }
  uint8_t visibleCapacity() const { return visibleCapacity_; }

  // No list to show at all: either the document has no lists, or the
  // current list has no items.
  bool empty() const { return listCount_ == 0 || itemCount_ == 0; }

 private:
  void clampWindow();

  uint16_t listIndex_ = 0;
  uint16_t listCount_ = 0;
  uint16_t itemCount_ = 0;
  uint16_t cursor_ = 0;
  uint16_t windowStart_ = 0;
  uint8_t visibleCapacity_ = 1;
};

}  // namespace companiontodo
