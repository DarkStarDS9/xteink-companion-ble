#include "CompanionTodoDiff.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using companiontodo::Diff;
using companiontodo::kMaxDiffEntries;

// Header is u8 version + u32 revision + u16 count; entries are 3 bytes each.
constexpr size_t kHeaderLen = 7;
constexpr size_t kEntryLen = 3;

std::vector<uint8_t> encodeToVector(const Diff& diff) {
  std::vector<uint8_t> buf(kHeaderLen + kEntryLen * diff.count());
  const size_t written = diff.encode(buf.data(), buf.size());
  EXPECT_EQ(written, buf.size());
  return buf;
}

TEST(CompanionTodoDiff, EmptyDiffDefersToDocument) {
  Diff diff;
  EXPECT_EQ(diff.count(), 0u);
  EXPECT_FALSE(diff.full());
  EXPECT_TRUE(diff.effectiveChecked(7, true));
  EXPECT_FALSE(diff.effectiveChecked(7, false));
}

TEST(CompanionTodoDiff, EntryOverridesDocumentBothDirections) {
  Diff diff;
  bool now = false;
  ASSERT_TRUE(diff.applyToggle(7, false, &now));
  EXPECT_TRUE(now);
  // The document still says unchecked; the diff is what the screen shows.
  EXPECT_TRUE(diff.effectiveChecked(7, false));

  Diff other;
  ASSERT_TRUE(other.applyToggle(7, true, &now));
  EXPECT_FALSE(now);
  EXPECT_FALSE(other.effectiveChecked(7, true));
}

TEST(CompanionTodoDiff, MissingIdInNonEmptyDiffDefersToDocument) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(10, false, nullptr));
  ASSERT_TRUE(diff.applyToggle(20, false, nullptr));
  ASSERT_TRUE(diff.applyToggle(30, false, nullptr));
  ASSERT_EQ(diff.count(), 3u);

  // Below the lowest, between two entries, and above the highest -- the three
  // ways a binary search can miss.
  EXPECT_FALSE(diff.effectiveChecked(5, false));
  EXPECT_TRUE(diff.effectiveChecked(5, true));
  EXPECT_FALSE(diff.effectiveChecked(25, false));
  EXPECT_TRUE(diff.effectiveChecked(25, true));
  EXPECT_FALSE(diff.effectiveChecked(99, false));
  EXPECT_TRUE(diff.effectiveChecked(99, true));
}

TEST(CompanionTodoDiff, TogglingUncheckedDocumentItemRecordsIt) {
  Diff diff;
  bool now = false;
  ASSERT_TRUE(diff.applyToggle(42, false, &now));
  EXPECT_TRUE(now);
  EXPECT_EQ(diff.count(), 1u);

  uint16_t id = 0;
  bool checked = false;
  ASSERT_TRUE(diff.entryAt(0, &id, &checked));
  EXPECT_EQ(id, 42u);
  EXPECT_TRUE(checked);
}

TEST(CompanionTodoDiff, TogglingBackToDocumentValueRemovesTheEntry) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(42, false, nullptr));
  bool now = true;
  ASSERT_TRUE(diff.applyToggle(42, false, &now));
  EXPECT_FALSE(now);
  // Removal, not a {42,false} entry: the diff records deviations only.
  EXPECT_EQ(diff.count(), 0u);
  EXPECT_FALSE(diff.entryAt(0, nullptr, nullptr));
  EXPECT_FALSE(diff.effectiveChecked(42, false));
}

TEST(CompanionTodoDiff, UncheckingACheckedDocumentItemIsARealEdit) {
  Diff diff;
  bool now = true;
  ASSERT_TRUE(diff.applyToggle(42, true, &now));
  EXPECT_FALSE(now);
  EXPECT_EQ(diff.count(), 1u);

  uint16_t id = 0;
  bool checked = true;
  ASSERT_TRUE(diff.entryAt(0, &id, &checked));
  EXPECT_EQ(id, 42u);
  EXPECT_FALSE(checked);
}

TEST(CompanionTodoDiff, ReCheckingACheckedDocumentItemRemovesTheEntry) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(42, true, nullptr));
  bool now = false;
  ASSERT_TRUE(diff.applyToggle(42, true, &now));
  EXPECT_TRUE(now);
  EXPECT_EQ(diff.count(), 0u);
  EXPECT_TRUE(diff.effectiveChecked(42, true));
}

TEST(CompanionTodoDiff, RepeatedTogglesNeverAccumulateEntries) {
  Diff diff;
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(diff.applyToggle(3, false, nullptr));
    EXPECT_LE(diff.count(), 1u);
  }
  EXPECT_EQ(diff.count(), 0u);  // even number of toggles: back to the document
}

TEST(CompanionTodoDiff, EntriesStayAscendingUnderOutOfOrderInsertion) {
  Diff diff;
  const uint16_t ids[] = {500, 3, 900, 1, 77, 250};
  for (uint16_t id : ids) ASSERT_TRUE(diff.applyToggle(id, false, nullptr));
  ASSERT_EQ(diff.count(), 6u);

  uint16_t previous = 0;
  for (uint16_t i = 0; i < diff.count(); ++i) {
    uint16_t id = 0;
    bool checked = false;
    ASSERT_TRUE(diff.entryAt(i, &id, &checked));
    if (i > 0) EXPECT_GT(id, previous);
    previous = id;
  }
}

TEST(CompanionTodoDiff, InsertAtCapacityFailsAndMutatesNothing) {
  Diff diff;
  for (uint16_t i = 0; i < kMaxDiffEntries; ++i) ASSERT_TRUE(diff.applyToggle(i, false, nullptr));
  ASSERT_EQ(diff.count(), kMaxDiffEntries);
  EXPECT_TRUE(diff.full());

  const std::vector<uint8_t> before = encodeToVector(diff);

  bool now = false;
  EXPECT_FALSE(diff.applyToggle(kMaxDiffEntries + 5, false, &now));
  EXPECT_EQ(diff.count(), kMaxDiffEntries);
  EXPECT_FALSE(diff.effectiveChecked(kMaxDiffEntries + 5, false));
  EXPECT_EQ(encodeToVector(diff), before);
}

TEST(CompanionTodoDiff, RemovalAtCapacityFreesASlot) {
  Diff diff;
  for (uint16_t i = 0; i < kMaxDiffEntries; ++i) ASSERT_TRUE(diff.applyToggle(i, false, nullptr));
  ASSERT_TRUE(diff.full());

  // Toggling an existing entry back to its document value is a removal, so it
  // succeeds even with the table full.
  ASSERT_TRUE(diff.applyToggle(0, false, nullptr));
  EXPECT_EQ(diff.count(), kMaxDiffEntries - 1);
  EXPECT_FALSE(diff.full());

  ASSERT_TRUE(diff.applyToggle(60000, false, nullptr));
  EXPECT_EQ(diff.count(), kMaxDiffEntries);
  EXPECT_TRUE(diff.effectiveChecked(60000, false));
}

TEST(CompanionTodoDiff, RoundTripsEmpty) {
  Diff diff;
  diff.setRevision(0xDEADBEEF);
  const std::vector<uint8_t> bytes = encodeToVector(diff);
  ASSERT_EQ(bytes.size(), kHeaderLen);

  Diff out;
  ASSERT_TRUE(Diff::decode(bytes.data(), bytes.size(), out));
  EXPECT_EQ(out.revision(), 0xDEADBEEFu);
  EXPECT_EQ(out.count(), 0u);
}

TEST(CompanionTodoDiff, RoundTripsSingleEntry) {
  Diff diff;
  diff.setRevision(9);
  ASSERT_TRUE(diff.applyToggle(1234, true, nullptr));
  const std::vector<uint8_t> bytes = encodeToVector(diff);

  Diff out;
  ASSERT_TRUE(Diff::decode(bytes.data(), bytes.size(), out));
  EXPECT_EQ(out.revision(), 9u);
  ASSERT_EQ(out.count(), 1u);
  uint16_t id = 0;
  bool checked = true;
  ASSERT_TRUE(out.entryAt(0, &id, &checked));
  EXPECT_EQ(id, 1234u);
  EXPECT_FALSE(checked);
}

TEST(CompanionTodoDiff, RoundTripsAFullTable) {
  Diff diff;
  diff.setRevision(0x01020304);
  for (uint16_t i = 0; i < kMaxDiffEntries; ++i) {
    // Alternate the document value so both checked states are exercised.
    ASSERT_TRUE(diff.applyToggle(static_cast<uint16_t>(i * 3), (i % 2) == 0, nullptr));
  }
  ASSERT_EQ(diff.count(), kMaxDiffEntries);
  const std::vector<uint8_t> bytes = encodeToVector(diff);
  ASSERT_EQ(bytes.size(), kHeaderLen + kEntryLen * kMaxDiffEntries);

  Diff out;
  ASSERT_TRUE(Diff::decode(bytes.data(), bytes.size(), out));
  EXPECT_EQ(out.revision(), 0x01020304u);
  ASSERT_EQ(out.count(), kMaxDiffEntries);
  for (uint16_t i = 0; i < kMaxDiffEntries; ++i) {
    uint16_t wantId = 0, gotId = 0;
    bool wantChecked = false, gotChecked = false;
    ASSERT_TRUE(diff.entryAt(i, &wantId, &wantChecked));
    ASSERT_TRUE(out.entryAt(i, &gotId, &gotChecked));
    EXPECT_EQ(gotId, wantId);
    EXPECT_EQ(gotChecked, wantChecked);
  }
}

TEST(CompanionTodoDiff, EncodeIntoUndersizedBufferWritesNothing) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(5, false, nullptr));

  uint8_t buf[kHeaderLen + kEntryLen];
  memset(buf, 0xAA, sizeof(buf));
  EXPECT_EQ(diff.encode(buf, sizeof(buf) - 1), 0u);
  for (uint8_t b : buf) EXPECT_EQ(b, 0xAA);
}

TEST(CompanionTodoDiff, DecodeRejectsTruncatedHeader) {
  Diff diff;
  const std::vector<uint8_t> bytes = encodeToVector(diff);
  Diff out;
  for (size_t len = 0; len < kHeaderLen; ++len) {
    EXPECT_FALSE(Diff::decode(bytes.data(), len, out)) << "len=" << len;
  }
}

TEST(CompanionTodoDiff, DecodeRejectsCountLongerThanTheBody) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(5, false, nullptr));
  std::vector<uint8_t> bytes = encodeToVector(diff);
  bytes[5] = 2;  // count low byte: claims two entries, one is present
  Diff out;
  EXPECT_FALSE(Diff::decode(bytes.data(), bytes.size(), out));
}

TEST(CompanionTodoDiff, DecodeRejectsWrongFormatVersion) {
  Diff diff;
  std::vector<uint8_t> bytes = encodeToVector(diff);
  bytes[0] = 2;
  Diff out;
  EXPECT_FALSE(Diff::decode(bytes.data(), bytes.size(), out));
}

TEST(CompanionTodoDiff, DecodeRejectsNonBooleanCheckedByte) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(5, false, nullptr));
  std::vector<uint8_t> bytes = encodeToVector(diff);
  bytes[kHeaderLen + 2] = 2;
  Diff out;
  EXPECT_FALSE(Diff::decode(bytes.data(), bytes.size(), out));
}

TEST(CompanionTodoDiff, DecodeRejectsUnsortedEntries) {
  Diff diff;
  ASSERT_TRUE(diff.applyToggle(5, false, nullptr));
  ASSERT_TRUE(diff.applyToggle(9, false, nullptr));
  std::vector<uint8_t> bytes = encodeToVector(diff);
  // Swap the two ids so they descend.
  std::swap(bytes[kHeaderLen], bytes[kHeaderLen + kEntryLen]);
  std::swap(bytes[kHeaderLen + 1], bytes[kHeaderLen + kEntryLen + 1]);
  Diff out;
  EXPECT_FALSE(Diff::decode(bytes.data(), bytes.size(), out));
}

TEST(CompanionTodoDiff, EncodedEntriesMatchEntryAtByteForByte) {
  Diff diff;
  diff.setRevision(0x11223344);
  const uint16_t ids[] = {1, 0x0102, 300, 0xFFFF};
  for (uint16_t id : ids) ASSERT_TRUE(diff.applyToggle(id, id == 300, nullptr));
  const std::vector<uint8_t> bytes = encodeToVector(diff);

  EXPECT_EQ(bytes[0], 1u);
  EXPECT_EQ(bytes[1], 0x44u);
  EXPECT_EQ(bytes[2], 0x33u);
  EXPECT_EQ(bytes[3], 0x22u);
  EXPECT_EQ(bytes[4], 0x11u);
  EXPECT_EQ(bytes[5], 4u);
  EXPECT_EQ(bytes[6], 0u);

  // This byte layout IS the LIST_STATE wire body; entryAt() is the same data
  // read structurally, so the two must agree at every offset.
  for (uint16_t k = 0; k < diff.count(); ++k) {
    uint16_t id = 0;
    bool checked = false;
    ASSERT_TRUE(diff.entryAt(k, &id, &checked));
    const size_t at = kHeaderLen + kEntryLen * k;
    EXPECT_EQ(bytes[at], static_cast<uint8_t>(id & 0xFF));
    EXPECT_EQ(bytes[at + 1], static_cast<uint8_t>(id >> 8));
    EXPECT_EQ(bytes[at + 2], checked ? 1u : 0u);
  }
}

TEST(CompanionTodoDiff, PageWalkAtProtocolPageSizeReproducesEveryEntry) {
  // The LIST_STATE pull is a page-at-a-time walk of exactly these bytes:
  // the device seeks to kHeaderLen + kEntryLen * offset and copies raw. That
  // arithmetic lives in CompanionPeerStore.cpp, which cannot be host-built --
  // but the property it depends on can be checked here, which is the point of
  // encode()'s wire-identical entry layout. If this test fails, no amount of
  // seeking on the device can produce a correct pull.
  constexpr uint16_t kEntriesPerNotify = 30;  // companionble::kListStateEntriesPerNotify

  Diff diff;
  diff.setRevision(7);
  // Deliberately more than two pages, and deliberately not a multiple of the
  // page size: a walk that only ever ends on a page boundary never exercises
  // the short final page, which is where an off-by-one hides.
  constexpr uint16_t kTotal = kEntriesPerNotify * 2 + 5;
  for (uint16_t i = 0; i < kTotal; ++i) ASSERT_TRUE(diff.applyToggle(static_cast<uint16_t>(1000 + i), i % 3 == 0, nullptr));
  const std::vector<uint8_t> bytes = encodeToVector(diff);

  std::vector<std::pair<uint16_t, bool>> walked;
  size_t pages = 0;
  for (uint16_t offset = 0;; offset += kEntriesPerNotify) {
    const uint16_t remaining = offset >= diff.count() ? 0 : static_cast<uint16_t>(diff.count() - offset);
    const uint16_t n = remaining < kEntriesPerNotify ? remaining : kEntriesPerNotify;
    ++pages;
    // The 11-byte LIST_STATE header plus the entries this page carries, which
    // is what must stay inside the session characteristic's 103-byte floor.
    EXPECT_LE(11u + 3u * n, 103u);
    if (n == 0) break;
    const size_t at = kHeaderLen + kEntryLen * offset;
    for (uint16_t k = 0; k < n; ++k) {
      const size_t entry = at + kEntryLen * k;
      walked.emplace_back(static_cast<uint16_t>(bytes[entry] | (bytes[entry + 1] << 8)), bytes[entry + 2] != 0);
    }
  }
  // Three full-or-partial pages plus the n = 0 terminator the phone stops on.
  EXPECT_EQ(pages, 4u);

  ASSERT_EQ(walked.size(), static_cast<size_t>(kTotal));
  for (uint16_t k = 0; k < kTotal; ++k) {
    uint16_t id = 0;
    bool checked = false;
    ASSERT_TRUE(diff.entryAt(k, &id, &checked));
    EXPECT_EQ(walked[k].first, id);
    EXPECT_EQ(walked[k].second, checked);
  }
}

TEST(CompanionTodoDiff, SizeIsBitPacked) {
  // The RAM justification for a resident 512-entry table: bit-packing `checked`
  // keeps this at 1096 bytes where a {u16,u8} struct array would pad to 2048.
  EXPECT_EQ(sizeof(Diff), 1096u);
}

}  // namespace
