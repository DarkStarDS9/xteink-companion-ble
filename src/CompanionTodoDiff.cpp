#include "CompanionTodoDiff.h"

#include <cstring>

namespace companiontodo {

namespace {

// Header: u8 formatVersion, u32 revision, u16 count. Entry: u16 itemId, u8 checked.
constexpr uint8_t kFormatVersion = 1;
constexpr size_t kHeaderLen = 7;
constexpr size_t kEntryLen = 3;

}  // namespace

void Diff::clear() {
  count_ = 0;
  revision_ = 0;
}

void Diff::setRevision(uint32_t r) { revision_ = r; }

uint16_t Diff::lowerBound(uint16_t itemId) const {
  uint16_t low = 0;
  uint16_t high = count_;
  while (low < high) {
    const uint16_t mid = static_cast<uint16_t>(low + (high - low) / 2);
    if (ids_[mid] < itemId) {
      low = static_cast<uint16_t>(mid + 1);
    } else {
      high = mid;
    }
  }
  return low;
}

bool Diff::checkedAt(uint16_t index) const { return (checkedBits_[index >> 3] >> (index & 7)) & 1u; }

void Diff::setCheckedAt(uint16_t index, bool checked) {
  const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
  if (checked) {
    checkedBits_[index >> 3] |= mask;
  } else {
    checkedBits_[index >> 3] &= static_cast<uint8_t>(~mask);
  }
}

void Diff::eraseAt(uint16_t index) {
  // The bits are indexed by entry position, not by itemId, so they shift with
  // the ids rather than staying put.
  for (uint16_t i = index; i + 1 < count_; ++i) setCheckedAt(i, checkedAt(static_cast<uint16_t>(i + 1)));
  memmove(&ids_[index], &ids_[index + 1], sizeof(uint16_t) * (count_ - index - 1));
  --count_;
}

void Diff::insertAt(uint16_t index, uint16_t itemId, bool checked) {
  for (uint16_t i = count_; i > index; --i) setCheckedAt(i, checkedAt(static_cast<uint16_t>(i - 1)));
  memmove(&ids_[index + 1], &ids_[index], sizeof(uint16_t) * (count_ - index));
  ids_[index] = itemId;
  setCheckedAt(index, checked);
  ++count_;
}

bool Diff::effectiveChecked(uint16_t itemId, bool documentChecked) const {
  const uint16_t index = lowerBound(itemId);
  if (index >= count_ || ids_[index] != itemId) return documentChecked;
  return checkedAt(index);
}

bool Diff::applyToggle(uint16_t itemId, bool documentChecked, bool* newCheckedOut) {
  const uint16_t index = lowerBound(itemId);
  const bool present = index < count_ && ids_[index] == itemId;
  const bool before = present ? checkedAt(index) : documentChecked;
  const bool after = !before;

  // Back to what the document says: the deviation is gone, so the entry is too.
  // This branch never needs a slot, which is why it succeeds even when full().
  if (after == documentChecked) {
    if (present) eraseAt(index);
    if (newCheckedOut) *newCheckedOut = after;
    return true;
  }

  if (present) {
    setCheckedAt(index, after);
    if (newCheckedOut) *newCheckedOut = after;
    return true;
  }

  // Refuse rather than evict: an edit the user made is not ours to drop, and a
  // caller that gets false can say so on screen. Nothing above this point
  // mutated anything.
  if (full()) return false;

  insertAt(index, itemId, after);
  if (newCheckedOut) *newCheckedOut = after;
  return true;
}

bool Diff::entryAt(uint16_t index, uint16_t* itemIdOut, bool* checkedOut) const {
  if (index >= count_) return false;
  if (itemIdOut) *itemIdOut = ids_[index];
  if (checkedOut) *checkedOut = checkedAt(index);
  return true;
}

size_t Diff::encode(uint8_t* out, size_t outLen) const {
  const size_t needed = kHeaderLen + kEntryLen * count_;
  if (!out || outLen < needed) return 0;

  out[0] = kFormatVersion;
  out[1] = static_cast<uint8_t>(revision_ & 0xFF);
  out[2] = static_cast<uint8_t>((revision_ >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>((revision_ >> 16) & 0xFF);
  out[4] = static_cast<uint8_t>((revision_ >> 24) & 0xFF);
  out[5] = static_cast<uint8_t>(count_ & 0xFF);
  out[6] = static_cast<uint8_t>((count_ >> 8) & 0xFF);

  size_t at = kHeaderLen;
  for (uint16_t i = 0; i < count_; ++i) {
    out[at] = static_cast<uint8_t>(ids_[i] & 0xFF);
    out[at + 1] = static_cast<uint8_t>(ids_[i] >> 8);
    out[at + 2] = checkedAt(i) ? 1 : 0;
    at += kEntryLen;
  }
  return needed;
}

bool Diff::decode(const uint8_t* in, size_t len, Diff& out) {
  out.clear();
  if (!in || len < kHeaderLen) return false;
  if (in[0] != kFormatVersion) return false;

  const uint32_t revision = static_cast<uint32_t>(in[1]) | (static_cast<uint32_t>(in[2]) << 8) |
                            (static_cast<uint32_t>(in[3]) << 16) | (static_cast<uint32_t>(in[4]) << 24);
  const uint16_t count = static_cast<uint16_t>(in[5] | (in[6] << 8));
  if (count > kMaxDiffEntries) return false;
  if (len - kHeaderLen < kEntryLen * count) return false;

  size_t at = kHeaderLen;
  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t itemId = static_cast<uint16_t>(in[at] | (in[at + 1] << 8));
    const uint8_t checked = in[at + 2];
    if (checked > 1) {
      out.clear();
      return false;
    }
    // Strictly ascending: see decode()'s header comment on why a duplicate or a
    // descent is a rejection and not something to sort out silently.
    if (i > 0 && itemId <= out.ids_[i - 1]) {
      out.clear();
      return false;
    }
    out.ids_[i] = itemId;
    out.setCheckedAt(i, checked != 0);
    out.count_ = static_cast<uint16_t>(i + 1);
    at += kEntryLen;
  }

  out.revision_ = revision;
  return true;
}

}  // namespace companiontodo
