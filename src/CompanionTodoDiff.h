#pragma once

#include <cstddef>
#include <cstdint>

// The on-device check-off diff for the ToDo List feature (Phase B) -- see
// docs/companion-todo-list-design.md. Mirrors CompanionTodoDocument.h's and
// CompanionTodoNav.h's charter exactly, and for the same reason: this must
// compile in the host test harness (test/companion_todo_diff/), so it may
// depend on nothing but <cstdint>/<cstddef>. No ArduinoJson, no HalStorage, no
// Arduino types, no allocation, no clock, ever -- CompanionPeerStore.cpp pulls
// those in and therefore cannot be host-built at all; nothing in here may
// acquire a dependency that changes that.
//
// WHY A DIFF AND NOT A MUTATED DOCUMENT: lists.bin holds the exact bytes the
// phone pushed, verbatim (CompanionPeerStore.h). Editing `checked` in place
// there would make the stored document a thing the device authored, and would
// destroy the phone's ability to tell what it sent from what the user did.
// Recording only the DEVIATIONS keeps the pushed document immutable and makes
// "what changed on the device" a first-class, directly-syncable object.
//
// WHY DEVIATIONS AND NOT ABSOLUTE STATES: an entry exists only while the user's
// value differs from the document's. Toggling an item back to what the document
// says REMOVES its entry rather than recording a redundant one, so a user
// fidgeting with the same checkbox cannot grow the table, and an empty diff
// means exactly "nothing to sync" without a comparison pass.
//
// MEMORY: sizeof(Diff) is 1096 bytes, and the class is designed around that
// number. `checked` is bit-packed rather than stored as a struct{u16,u8}[512],
// which the compiler would pad to 2048 -- nearly a kilobyte saved on a
// no-PSRAM part where NimBLE already costs ~63 KB on top of the 52,272-byte
// framebuffer (CLAUDE.md, "Memory reality"). 512 entries is the cap because
// kMaxListDocLen (16 KB) admits on the order of 500 short items, so the table
// can hold a deviation for every item a legal document can contain; a smaller
// cap would mean a user could be told "no more edits" on a document the
// firmware otherwise renders fine.
namespace companiontodo {

// Most deviations held at once. See the MEMORY note above for the derivation.
inline constexpr uint16_t kMaxDiffEntries = 512;

class Diff {
 public:
  // Drops every entry and resets the revision. Called when the peer pushes a
  // new document -- see CompanionPeerStore::clearListState().
  void clear();

  // The document revision these deviations were made against. The device never
  // interprets it; it is recorded so the phone can decide whether the diff it
  // pulls still applies to the document it sent (the device deciding that would
  // put a policy judgement on the dumb side of the split -- CLAUDE.md, "dumb
  // firmware, smart phone").
  void setRevision(uint32_t r);
  uint32_t revision() const { return revision_; }

  uint16_t count() const { return count_; }
  bool full() const { return count_ >= kMaxDiffEntries; }

  // The rendered checkbox: the document's value overridden by any local toggle.
  bool effectiveChecked(uint16_t itemId, bool documentChecked) const;

  // Confirm. Records only DEVIATIONS from documentChecked: flipping an item
  // back to its document value REMOVES its entry.
  //
  // Returns false only when a new entry was needed and the table is full --
  // nothing is mutated in that case, deliberately, so the caller can tell the
  // user the edit was refused instead of silently losing it or silently
  // evicting somebody else's. `newCheckedOut` (may be null) receives the value
  // now on screen; it is untouched on a refusal.
  bool applyToggle(uint16_t itemId, bool documentChecked, bool* newCheckedOut);

  // Reads the entry at `index` in ascending-itemId order. Both out-params may
  // be null. False when `index` is past the end.
  bool entryAt(uint16_t index, uint16_t* itemIdOut, bool* checkedOut) const;

  // Serialises to the list_state.bin body:
  //
  //   u8   formatVersion = 1
  //   u32  revision                         (little-endian)
  //   u16  count                            (little-endian)
  //   count x { u16 itemId (LE), u8 checked }   -- ascending by itemId
  //
  // 7 + 3n bytes. Little-endian, matching every other multi-byte field on this
  // wire (CompanionTodoDocument.h's ENDIANNESS note).
  //
  // The entry encoding is byte-identical to the LIST_STATE wire body, and that
  // identity is the point: the BLE layer can answer an offset-addressed pull by
  // seeking to 7 + 3*offset in the file and copying bytes straight out, with no
  // resident Diff and no re-encoding step to keep in sync with this one.
  // Ascending order is what makes that offset addressing stable across pulls.
  //
  // Returns the number of bytes written, or 0 -- writing nothing -- if `outLen`
  // is too small.
  size_t encode(uint8_t* out, size_t outLen) const;

  // Parses `in[0..len)` back into `out`, which is cleared first. Rejects a
  // truncated header, a formatVersion other than 1, a count that runs off the
  // end or exceeds kMaxDiffEntries, a `checked` byte that is not 0 or 1, and
  // entries that are not strictly ascending. That last one is a corruption
  // guard rather than a format rule: everything downstream binary-searches this
  // table, so out-of-order entries would not fail loudly, they would silently
  // return wrong checkbox states. `out` is left cleared on any rejection.
  static bool decode(const uint8_t* in, size_t len, Diff& out);

 private:
  // Index of the first entry with id >= itemId, i.e. where itemId is or would
  // be inserted.
  uint16_t lowerBound(uint16_t itemId) const;
  bool checkedAt(uint16_t index) const;
  void setCheckedAt(uint16_t index, bool checked);
  void eraseAt(uint16_t index);
  void insertAt(uint16_t index, uint16_t itemId, bool checked);

  uint16_t ids_[kMaxDiffEntries];  // sorted ascending; binary-searched
  uint8_t checkedBits_[(kMaxDiffEntries + 7) / 8];
  uint32_t revision_ = 0;
  uint16_t count_ = 0;
};

}  // namespace companiontodo
