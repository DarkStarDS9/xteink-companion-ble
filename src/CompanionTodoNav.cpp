#include "CompanionTodoNav.h"

#include <algorithm>

namespace companiontodo {

void Nav::reset() {
  listIndex_ = 0;
  listCount_ = 0;
  itemCount_ = 0;
  cursor_ = 0;
  windowStart_ = 0;
  // visibleCapacity_ is a viewport property, not document state -- left alone.
}

void Nav::setListCount(uint16_t listCount) {
  listCount_ = listCount;
  if (listCount_ == 0) {
    listIndex_ = 0;
    itemCount_ = 0;
    cursor_ = 0;
    windowStart_ = 0;
    return;
  }
  if (listIndex_ >= listCount_) listIndex_ = listCount_ - 1;
}

void Nav::setCurrentList(uint16_t itemCount) {
  itemCount_ = itemCount;
  if (itemCount_ == 0) {
    cursor_ = 0;
    windowStart_ = 0;
    return;
  }
  if (cursor_ >= itemCount_) cursor_ = itemCount_ - 1;
  clampWindow();
}

void Nav::setVisibleCapacity(uint8_t capacity) {
  visibleCapacity_ = capacity == 0 ? 1 : capacity;
  clampWindow();
}

void Nav::clampWindow() {
  if (itemCount_ == 0) {
    windowStart_ = 0;
    return;
  }
  // Keep the cursor inside [windowStart, windowStart + capacity).
  if (cursor_ < windowStart_) windowStart_ = cursor_;
  if (cursor_ >= windowStart_ + visibleCapacity_) {
    windowStart_ = cursor_ - visibleCapacity_ + 1;
  }
  // Don't scroll past the point where the last page would show blank rows
  // for a list just long enough to fill less than one extra page.
  const uint16_t maxStart = itemCount_ > visibleCapacity_ ? itemCount_ - visibleCapacity_ : 0;
  if (windowStart_ > maxStart) windowStart_ = maxStart;
}

bool Nav::moveUp() {
  if (itemCount_ == 0 || cursor_ == 0) return false;
  cursor_--;
  clampWindow();
  return true;
}

bool Nav::moveDown() {
  if (itemCount_ == 0 || cursor_ + 1 >= itemCount_) return false;
  cursor_++;
  clampWindow();
  return true;
}

bool Nav::switchListLeft() {
  if (listCount_ < 2) return false;
  listIndex_ = listIndex_ == 0 ? listCount_ - 1 : listIndex_ - 1;
  cursor_ = 0;
  windowStart_ = 0;
  return true;
}

bool Nav::switchListRight() {
  if (listCount_ < 2) return false;
  listIndex_ = listIndex_ + 1 >= listCount_ ? 0 : listIndex_ + 1;
  cursor_ = 0;
  windowStart_ = 0;
  return true;
}

}  // namespace companiontodo
