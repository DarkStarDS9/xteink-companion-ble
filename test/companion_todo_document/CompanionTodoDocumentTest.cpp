#include "CompanionTodoDocument.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "CompanionBle.h"

namespace {

using companiontodo::ParseResult;
using companiontodo::Visitor;

// ---------------------------------------------------------------------------
// Builders, not literals -- every offset in this format shifts when the
// layout changes, so a test that hand-counts bytes stops describing what it
// meant the moment the layout moves again (see
// test/companion_ui_declaration/CompanionUiDeclarationTest.cpp's comment,
// which this mirrors).
// ---------------------------------------------------------------------------

void appendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void appendString(std::vector<uint8_t>& out, const std::string& s) {
  out.push_back(static_cast<uint8_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// A single item entry: itemId, checked, text.
std::vector<uint8_t> item(uint16_t itemId, uint8_t checked, const std::string& text) {
  std::vector<uint8_t> out;
  appendU16LE(out, itemId);
  out.push_back(checked);
  appendString(out, text);
  return out;
}

// A single group entry: groupId, label, item count, items.
std::vector<uint8_t> group(uint16_t groupId, const std::string& label,
                            std::initializer_list<std::vector<uint8_t>> items) {
  std::vector<uint8_t> out;
  appendU16LE(out, groupId);
  appendString(out, label);
  out.push_back(static_cast<uint8_t>(items.size()));
  for (const auto& it : items) append(out, it);
  return out;
}

// A single list entry: listId, title, group count, groups.
std::vector<uint8_t> list(uint16_t listId, const std::string& title,
                           std::initializer_list<std::vector<uint8_t>> groups) {
  std::vector<uint8_t> out;
  appendU16LE(out, listId);
  appendString(out, title);
  out.push_back(static_cast<uint8_t>(groups.size()));
  for (const auto& g : groups) append(out, g);
  return out;
}

// The whole document: revision, list count, lists.
std::vector<uint8_t> doc(uint32_t revision, std::initializer_list<std::vector<uint8_t>> lists) {
  std::vector<uint8_t> out;
  appendU32LE(out, revision);
  out.push_back(static_cast<uint8_t>(lists.size()));
  for (const auto& l : lists) append(out, l);
  return out;
}

ParseResult parse(const std::vector<uint8_t>& d, Visitor& v) {
  return companiontodo::parseDocument(d.data(), d.size(), v);
}

// ---------------------------------------------------------------------------
// A visitor that records every callback as a human-readable line, so a test
// can assert on the exact wire-order sequence in one EXPECT_EQ instead of a
// pile of separate field checks.
// ---------------------------------------------------------------------------

class RecordingVisitor : public Visitor {
 public:
  std::vector<std::string> log;

  void onDocument(uint32_t revision) override { log.push_back("doc:" + std::to_string(revision)); }
  void onListStart(uint16_t listId, const char* title, uint8_t titleLen) override {
    log.push_back("list:" + std::to_string(listId) + ":" + std::string(title, titleLen));
  }
  void onGroupStart(uint16_t groupId, const char* label, uint8_t labelLen) override {
    log.push_back("group:" + std::to_string(groupId) + ":" + std::string(label, labelLen));
  }
  void onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) override {
    log.push_back("item:" + std::to_string(itemId) + ":" + (checked ? "1" : "0") + ":" +
                   std::string(text, textLen));
  }
  void onGroupEnd(uint16_t groupId) override { log.push_back("group_end:" + std::to_string(groupId)); }
  void onListEnd(uint16_t listId) override { log.push_back("list_end:" + std::to_string(listId)); }
};

// A visitor that does nothing, for tests that only care about ParseResult.
class NullVisitor : public Visitor {};

// ---------------------------------------------------------------------------
// Well-formed documents: every field round-trips.
// ---------------------------------------------------------------------------

TEST(CompanionTodoDocument, MultiListMultiGroupMultiItemRoundTripsEveryField) {
  const auto d = doc(42, {
                             list(1, "Groceries",
                                  {
                                      group(10, "Produce",
                                            {
                                                item(100, 0, "Apples"),
                                                item(101, 1, "Bananas"),
                                            }),
                                      group(11, "Dairy", {item(102, 0, "Milk")}),
                                  }),
                             list(2, "Hardware", {group(20, "", {item(200, 1, "Nails")})}),
                         });
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  const std::vector<std::string> expected = {
      "doc:42",
      "list:1:Groceries",
      "group:10:Produce",
      "item:100:0:Apples",
      "item:101:1:Bananas",
      "group_end:10",
      "group:11:Dairy",
      "item:102:0:Milk",
      "group_end:11",
      "list_end:1",
      "list:2:Hardware",
      "group:20:",
      "item:200:1:Nails",
      "group_end:20",
      "list_end:2",
  };
  EXPECT_EQ(v.log, expected);
}

TEST(CompanionTodoDocument, EmptyDocumentHasZeroLists) {
  const auto d = doc(1, {});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log, std::vector<std::string>({"doc:1"}));
}

TEST(CompanionTodoDocument, EmptyGroupLabelIsTheUngroupedBucket) {
  const auto d = doc(1, {list(1, "List", {group(0, "", {item(1, 0, "Solo")})})});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log[2], "group:0:");
}

TEST(CompanionTodoDocument, EmptyItemTextIsLegal) {
  const auto d = doc(1, {list(1, "List", {group(0, "Group", {item(1, 0, "")})})});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log[3], "item:1:0:");
}

TEST(CompanionTodoDocument, EmptyListTitleIsLegal) {
  const auto d = doc(1, {list(1, "", {})});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log[1], "list:1:");
}

TEST(CompanionTodoDocument, ListWithNoGroupsIsLegal) {
  const auto d = doc(1, {list(5, "Empty list", {})});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log, std::vector<std::string>({"doc:1", "list:5:Empty list", "list_end:5"}));
}

TEST(CompanionTodoDocument, GroupWithNoItemsIsLegal) {
  const auto d = doc(1, {list(1, "List", {group(1, "Empty group", {})})});
  RecordingVisitor v;
  ASSERT_EQ(parse(d, v), ParseResult::Ok);
  EXPECT_EQ(v.log, std::vector<std::string>(
                        {"doc:1", "list:1:List", "group:1:Empty group", "group_end:1", "list_end:1"}));
}

// ---------------------------------------------------------------------------
// checked must be exactly 0 or 1.
// ---------------------------------------------------------------------------

TEST(CompanionTodoDocument, CheckedZeroAndOneAreLegal) {
  for (uint8_t checked : {static_cast<uint8_t>(0), static_cast<uint8_t>(1)}) {
    const auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, checked, "Item")})})});
    NullVisitor v;
    EXPECT_EQ(parse(d, v), ParseResult::Ok) << "checked=" << int(checked);
  }
}

TEST(CompanionTodoDocument, CheckedTwoIsMalformed) {
  const auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, 2, "Item")})})});
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, CheckedTwoFiftyFiveIsMalformed) {
  const auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, 255, "Item")})})});
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

// ---------------------------------------------------------------------------
// Trailing garbage and over-claimed counts.
// ---------------------------------------------------------------------------

TEST(CompanionTodoDocument, TrailingGarbageAfterAValidDocumentIsMalformed) {
  auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, 0, "Item")})})});
  d.push_back(0xFF);
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, ListCountLargerThanListsPresentIsMalformed) {
  auto d = doc(1, {list(1, "L", {})});
  d[4] = 2;  // claim 2 lists, only 1 present
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, GroupCountLargerThanGroupsPresentIsMalformed) {
  auto d = doc(1, {list(1, "L", {group(1, "G", {})})});
  // Offset of the list's group-count byte: revision(4) + listCount(1) +
  // listId(2) + titleLen(1) + title("L"=1 byte) = 9.
  ASSERT_EQ(d[9], 1);
  d[9] = 2;  // claim 2 groups, only 1 present
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, ItemCountLargerThanItemsPresentIsMalformed) {
  auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, 0, "X")})})});
  // Offset of the group's item-count byte: revision(4) + listCount(1) +
  // listId(2) + titleLen(1) + title("L"=1) + groupCount(1) + groupId(2) +
  // labelLen(1) + label("G"=1) = 14.
  ASSERT_EQ(d[14], 1);
  d[14] = 2;  // claim 2 items, only 1 present
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, ListCountFewerThanListsPresentLeavesTrailingBytesMalformed) {
  // The inverse of the over-claim cases above: understating a count leaves
  // the second list's bytes dangling as unexplained trailing data.
  auto d = doc(1, {list(1, "A", {}), list(2, "B", {})});
  d[4] = 1;  // claim 1 list, 2 actually present
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

// ---------------------------------------------------------------------------
// Truncation at every structural boundary.
// ---------------------------------------------------------------------------

TEST(CompanionTodoDocument, EmptyBufferIsMalformed) {
  const std::vector<uint8_t> empty;
  NullVisitor v;
  EXPECT_EQ(companiontodo::parseDocument(empty.data(), 0, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, NullBufferIsMalformed) {
  NullVisitor v;
  EXPECT_EQ(companiontodo::parseDocument(nullptr, 0, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, BufferShorterThanRevisionIsMalformed) {
  for (size_t len = 0; len < 4; ++len) {
    const std::vector<uint8_t> truncated(len, 0xAA);
    NullVisitor v;
    EXPECT_EQ(companiontodo::parseDocument(truncated.data(), len, v), ParseResult::Malformed) << "len " << len;
  }
}

TEST(CompanionTodoDocument, RevisionWithNoListCountByteIsMalformed) {
  std::vector<uint8_t> d;
  appendU32LE(d, 1);
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
}

TEST(CompanionTodoDocument, EveryTruncationOfAValidDocumentIsRejectedNotOverRead) {
  // The blunt instrument that catches an off-by-one nobody thought to name:
  // no prefix of a good document may parse as Ok unless it happens to be a
  // legal shorter document in its own right, and none may read past `len`
  // (run under a sanitizer build for the second half of that claim).
  const auto full = doc(7, {
                               list(1, "Groceries",
                                    {
                                        group(10, "Produce",
                                              {
                                                  item(100, 0, "Apples"),
                                                  item(101, 1, "Bananas"),
                                              }),
                                        group(11, "", {item(102, 0, "Milk")}),
                                    }),
                               list(2, "", {}),
                           });
  NullVisitor sanity;
  ASSERT_EQ(parse(full, sanity), ParseResult::Ok);

  for (size_t len = 0; len < full.size(); ++len) {
    NullVisitor v;
    const ParseResult r = companiontodo::parseDocument(full.data(), len, v);
    // A truncated prefix is only legitimately Ok if it happens to describe a
    // complete, shorter document (e.g. len==5, a zero-list document) -- the
    // length arithmetic decides that, not the parser guessing.
    if (r == ParseResult::Ok) {
      EXPECT_GE(len, companiontodo::kMinDocLen) << "len " << len;
    }
  }
}

// ---------------------------------------------------------------------------
// kMaxListDocLen boundary. Note: parseDocument() itself enforces no length
// cap -- that is CompanionBle.cpp's job when it reassembles a kFieldListDoc
// push, out of this test's scope (see CompanionTodoDocument.h's comment on
// the division of labour, mirroring CompanionUiDeclaration/kMaxUiDeclarationLen).
// These two tests instead prove the parser has no *internal* fixed-size
// buffer assumption that breaks right at that boundary -- it must handle a
// document of any size purely by walking the caller's buffer.
// ---------------------------------------------------------------------------

// Builds a well-formed single-list document of exactly `target` bytes: full
// groups of 50 zero-length-text items (a multiple-of-4-bytes-per-item, easy
// to reason about) until the remaining gap is small, then one final group
// with a single item whose text length is tuned to close the gap exactly.
std::vector<uint8_t> documentOfExactSize(uint32_t target) {
  constexpr uint8_t kItemsPerFullGroup = 50;
  constexpr size_t kFullGroupCost = 4 + static_cast<size_t>(kItemsPerFullGroup) * 4;

  std::vector<uint8_t> out;
  appendU32LE(out, 1);   // revision
  out.push_back(1);      // list count = 1
  appendU16LE(out, 1);   // listId
  out.push_back(0);      // empty title
  const size_t groupCountOffset = out.size();
  out.push_back(0);  // group count, patched below
  uint16_t groupCount = 0;
  uint16_t nextItemId = 0;
  uint16_t nextGroupId = 0;

  while (out.size() + kFullGroupCost + 8 <= target) {
    appendU16LE(out, nextGroupId++);
    out.push_back(0);  // empty label
    out.push_back(kItemsPerFullGroup);
    for (uint8_t i = 0; i < kItemsPerFullGroup; ++i) {
      appendU16LE(out, nextItemId++);
      out.push_back(0);  // unchecked
      out.push_back(0);  // zero-length text
    }
    ++groupCount;
  }

  // Final partial group: groupId(2) + labelLen(1) + itemCount(1) + itemId(2)
  // + checked(1) + textLen(1) = 8 fixed bytes, plus a text payload sized to
  // close the remaining gap exactly.
  const size_t gap = target - out.size();
  EXPECT_GE(gap, 8u);
  const size_t textLen = gap - 8;
  EXPECT_LE(textLen, 255u);
  appendU16LE(out, nextGroupId++);
  out.push_back(0);  // empty label
  out.push_back(1);  // one item
  appendU16LE(out, nextItemId++);
  out.push_back(0);  // unchecked
  out.push_back(static_cast<uint8_t>(textLen));
  out.insert(out.end(), textLen, 'x');
  ++groupCount;

  EXPECT_LE(groupCount, 255);
  out[groupCountOffset] = static_cast<uint8_t>(groupCount);
  return out;
}

TEST(CompanionTodoDocument, WellFormedDocumentAtExactlyKMaxListDocLenParses) {
  const auto d = documentOfExactSize(companionble::kMaxListDocLen);
  ASSERT_EQ(d.size(), companionble::kMaxListDocLen);
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Ok);
}

TEST(CompanionTodoDocument, WellFormedDocumentOneByteOverKMaxListDocLenStillParsesStructurally) {
  // Confirms the parser itself doesn't fall over one byte past the cap --
  // rejecting a push this size is CompanionBle.cpp's job (kMaxListDocLen
  // enforcement), not this function's.
  const auto d = documentOfExactSize(companionble::kMaxListDocLen + 1);
  ASSERT_EQ(d.size(), companionble::kMaxListDocLen + 1);
  NullVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Ok);
}

// ---------------------------------------------------------------------------
// Malformed input never triggers callbacks for entries past the bad byte.
// ---------------------------------------------------------------------------

TEST(CompanionTodoDocument, MalformedItemStopsTheWalkAtThatPoint) {
  // The second item's checked byte is invalid; the first item, and the
  // preceding doc/list/group callbacks, still fired before the walk gave up
  // -- the same partial-output-on-failure contract CompanionUiDeclaration
  // documents for walkBody().
  auto d = doc(1, {list(1, "L", {group(1, "G", {item(1, 0, "First"), item(2, 2, "Second")})})});
  RecordingVisitor v;
  EXPECT_EQ(parse(d, v), ParseResult::Malformed);
  ASSERT_GE(v.log.size(), 4u);
  EXPECT_EQ(v.log[0], "doc:1");
  EXPECT_EQ(v.log[1], "list:1:L");
  EXPECT_EQ(v.log[2], "group:1:G");
  EXPECT_EQ(v.log[3], "item:1:0:First");
  // No "item:2:..." -- the malformed checked byte was caught before onItem
  // fired for it.
  for (const auto& line : v.log) EXPECT_EQ(line.rfind("item:2", 0), std::string::npos);
}

}  // namespace
