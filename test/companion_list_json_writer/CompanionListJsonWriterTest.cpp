#include "CompanionListJsonWriter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "CompanionBle.h"
#include "CompanionTodoDocument.h"

namespace {

using companionpeer::JsonListWriter;
using companionpeer::JsonWriteSink;

// ---------------------------------------------------------------------------
// Wire-document builders, mirroring
// test/companion_todo_document/CompanionTodoDocumentTest.cpp's own (not
// shared between the two -- each is a handful of lines and duplicating them
// keeps this suite free of a cross-test-binary dependency).
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

std::vector<uint8_t> item(uint16_t itemId, uint8_t checked, const std::string& text) {
  std::vector<uint8_t> out;
  appendU16LE(out, itemId);
  out.push_back(checked);
  appendString(out, text);
  return out;
}

std::vector<uint8_t> group(uint16_t groupId, const std::string& label,
                            std::initializer_list<std::vector<uint8_t>> items) {
  std::vector<uint8_t> out;
  appendU16LE(out, groupId);
  appendString(out, label);
  out.push_back(static_cast<uint8_t>(items.size()));
  for (const auto& it : items) append(out, it);
  return out;
}

// Same shape as group(), but takes items already assembled (so a caller can
// pass more than 255 via a loop building the vector directly) -- used by the
// item-cap test below, which needs several hundred items across a few
// groups.
std::vector<uint8_t> groupOf(uint16_t groupId, const std::string& label, const std::vector<std::vector<uint8_t>>& items) {
  std::vector<uint8_t> out;
  appendU16LE(out, groupId);
  appendString(out, label);
  out.push_back(static_cast<uint8_t>(items.size()));
  for (const auto& it : items) append(out, it);
  return out;
}

std::vector<uint8_t> list(uint16_t listId, const std::string& title,
                           std::initializer_list<std::vector<uint8_t>> groups) {
  std::vector<uint8_t> out;
  appendU16LE(out, listId);
  appendString(out, title);
  out.push_back(static_cast<uint8_t>(groups.size()));
  for (const auto& g : groups) append(out, g);
  return out;
}

std::vector<uint8_t> listOf(uint16_t listId, const std::string& title, const std::vector<std::vector<uint8_t>>& groups) {
  std::vector<uint8_t> out;
  appendU16LE(out, listId);
  appendString(out, title);
  out.push_back(static_cast<uint8_t>(groups.size()));
  for (const auto& g : groups) append(out, g);
  return out;
}

std::vector<uint8_t> doc(uint32_t revision, std::initializer_list<std::vector<uint8_t>> lists) {
  std::vector<uint8_t> out;
  appendU32LE(out, revision);
  out.push_back(static_cast<uint8_t>(lists.size()));
  for (const auto& l : lists) append(out, l);
  return out;
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

// Appends every flushed chunk to `out`, so a test can compare the whole
// stream at once. Records each individual write() call's length too, so a
// test can assert on flush boundaries directly (kBufferSize-sized chunks,
// with a final short one).
class StringSink : public JsonWriteSink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    out.append(reinterpret_cast<const char*>(data), len);
    writeLens.push_back(len);
    return true;
  }

  std::string out;
  std::vector<size_t> writeLens;
};

// Fails starting at the Nth write() call (0-indexed), simulating a full SD
// card mid-document -- the scenario storeListDocument()'s comment on ok()
// calls out explicitly.
class FailingSink : public JsonWriteSink {
 public:
  explicit FailingSink(size_t failAt) : failAt_(failAt) {}

  bool write(const uint8_t* data, size_t len) override {
    out.append(reinterpret_cast<const char*>(data), len);
    const bool succeed = calls_ < failAt_;
    ++calls_;
    return succeed;
  }

  std::string out;

 private:
  size_t failAt_;
  size_t calls_ = 0;
};

void driveDocument(const std::vector<uint8_t>& wire, JsonListWriter& writer) {
  const companiontodo::ParseResult result = companiontodo::parseDocument(wire.data(), wire.size(), writer);
  ASSERT_EQ(result, companiontodo::ParseResult::Ok);
  writer.finish();
}

// ---------------------------------------------------------------------------
// Shape and escaping
// ---------------------------------------------------------------------------

TEST(CompanionListJsonWriter, EmptyDocument) {
  StringSink sink;
  JsonListWriter writer(sink);
  driveDocument(doc(7, {}), writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_FALSE(writer.overCap());
  EXPECT_EQ(sink.out, R"({"revision":7,"lists":[]})");
}

TEST(CompanionListJsonWriter, OneListGroupItem) {
  StringSink sink;
  JsonListWriter writer(sink);
  const auto wire = doc(1, {list(10, "Groceries", {group(20, "Produce", {item(30, 1, "Apples")})})});
  driveDocument(wire, writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_EQ(sink.out,
            R"({"revision":1,"lists":[{"listId":10,"title":"Groceries","groups":[)"
            R"({"groupId":20,"label":"Produce","items":[)"
            R"({"itemId":30,"text":"Apples","checked":true}]}]}]})");
}

TEST(CompanionListJsonWriter, MultipleSiblingsGetCommas) {
  StringSink sink;
  JsonListWriter writer(sink);
  const auto wire = doc(
      1, {list(1, "A",
                {group(1, "G1", {item(1, 0, "x"), item(2, 1, "y")}), group(2, "G2", {item(3, 0, "z")})}),
          list(2, "B", {})});
  driveDocument(wire, writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_EQ(sink.out,
            R"({"revision":1,"lists":[)"
            R"({"listId":1,"title":"A","groups":[)"
            R"({"groupId":1,"label":"G1","items":[{"itemId":1,"text":"x","checked":false},{"itemId":2,"text":"y","checked":true}]},)"
            R"({"groupId":2,"label":"G2","items":[{"itemId":3,"text":"z","checked":false}]}]},)"
            R"({"listId":2,"title":"B","groups":[]}]})");
}

TEST(CompanionListJsonWriter, EscapesQuoteBackslashAndWhitespaceControls) {
  StringSink sink;
  JsonListWriter writer(sink);
  const std::string text = "a\"b\\c\nd\re\tf";
  const auto wire = doc(1, {list(1, "T", {group(1, "", {item(1, 0, text)})})});
  driveDocument(wire, writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_NE(sink.out.find(R"("text":"a\"b\\c\nd\re\tf")"), std::string::npos) << sink.out;
}

TEST(CompanionListJsonWriter, EscapesOtherControlCharsAsUnicode) {
  StringSink sink;
  JsonListWriter writer(sink);
  const std::string text = std::string(1, static_cast<char>(0x01)) + std::string(1, static_cast<char>(0x1F));
  const auto wire = doc(1, {list(1, "T", {group(1, "", {item(1, 0, text)})})});
  driveDocument(wire, writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_NE(sink.out.find(R"("text":"\u0001\u001f")"), std::string::npos) << sink.out;
}

TEST(CompanionListJsonWriter, MultiByteUtf8PassesThroughVerbatim) {
  StringSink sink;
  JsonListWriter writer(sink);
  // "café" and a CJK snippet -- both entirely >= 0x20 per byte (no C0
  // control byte ever appears inside a valid UTF-8 continuation), so this
  // must pass through byte-for-byte unescaped, per the writer's own
  // documented contract (it validates JSON-required escapes only, never
  // UTF-8 well-formedness).
  const std::string text = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";
  const auto wire = doc(1, {list(1, "T", {group(1, "", {item(1, 0, text)})})});
  driveDocument(wire, writer);
  EXPECT_TRUE(writer.ok());
  EXPECT_NE(sink.out.find("\"text\":\"" + text + "\""), std::string::npos) << sink.out;
}

// ---------------------------------------------------------------------------
// Buffering: the whole point of this class. Verify a document whose output
// spans many buffer-fulls (kBufferSize = 256) still comes out byte-identical
// to the unbuffered shape, and that flush() only fires when the buffer is
// actually full (or at the very end) -- not more often than that.
// ---------------------------------------------------------------------------

TEST(CompanionListJsonWriter, LargeDocumentFlushesInFullBufferChunks) {
  StringSink sink;
  JsonListWriter writer(sink);

  // 40 items of 20 bytes of text each, comfortably spanning many multiples
  // of kBufferSize (256) in the rendered JSON, including boundaries that
  // land mid-item and mid-escape (the text below straddles the buffer with
  // an escaped character right at essentially every possible offset since
  // items differ in id/parity).
  std::vector<std::vector<uint8_t>> items;
  for (int i = 0; i < 40; ++i) {
    const std::string text = "line\"" + std::to_string(i) + "\\tail\n";  // forces quote/backslash/newline escapes
    items.push_back(item(static_cast<uint16_t>(i), i % 2, text));
  }
  const auto wire = doc(99, {listOf(1, "Big", {groupOf(1, "G", items)})});
  driveDocument(wire, writer);

  ASSERT_TRUE(writer.ok());
  ASSERT_GT(sink.out.size(), JsonListWriter::kBufferSize * 2)
      << "test is not actually exercising multiple buffer flushes";

  // Every write() call except possibly the last is exactly kBufferSize --
  // confirms flush() fires on "buffer full", not on some other cadence.
  ASSERT_FALSE(sink.writeLens.empty());
  for (size_t i = 0; i + 1 < sink.writeLens.size(); ++i) {
    EXPECT_EQ(sink.writeLens[i], JsonListWriter::kBufferSize) << "chunk " << i;
  }
  EXPECT_LE(sink.writeLens.back(), JsonListWriter::kBufferSize);

  // And the reassembled stream must still be exactly correct: every item's
  // text (with its escapes) appears, in order, undamaged by a flush landing
  // in the middle of it.
  for (int i = 0; i < 40; ++i) {
    const std::string expected =
        R"("itemId":)" + std::to_string(i) + R"(,"text":"line\")" + std::to_string(i) + R"(\\tail\n")";
    EXPECT_NE(sink.out.find(expected), std::string::npos) << "item " << i << " missing/corrupted:\n" << sink.out;
  }
}

// ---------------------------------------------------------------------------
// ok() propagation
// ---------------------------------------------------------------------------

TEST(CompanionListJsonWriter, SinkFailureLatchesNotOk) {
  // A document large enough to force at least two internal-buffer flushes
  // (kBufferSize = 256), so failAt=1 exercises a failure mid-document rather
  // than only in the final finish() flush -- FailureInFinalFlushIsCaught
  // below covers that narrower case separately.
  FailingSink sink(/*failAt=*/1);  // every write() call from the second on fails
  JsonListWriter writer(sink);
  std::vector<std::vector<uint8_t>> items;
  for (int i = 0; i < 40; ++i) items.push_back(item(static_cast<uint16_t>(i), 0, "line of text " + std::to_string(i)));
  const auto wire = doc(1, {listOf(1, "Big", {groupOf(1, "G", items)})});
  driveDocument(wire, writer);
  EXPECT_FALSE(writer.ok());
}

TEST(CompanionListJsonWriter, FailureInFinalFlushIsCaught) {
  // A document short enough to sit entirely in the internal buffer until
  // finish() flushes it -- the only write() call happens inside finish(),
  // so this specifically covers "check ok() AFTER finish()".
  FailingSink sink(/*failAt=*/0);  // every write() call fails
  JsonListWriter writer(sink);
  const auto wire = doc(1, {});
  driveDocument(wire, writer);
  EXPECT_FALSE(writer.ok());
}

// ---------------------------------------------------------------------------
// Item cap
// ---------------------------------------------------------------------------

TEST(CompanionListJsonWriter, ReportsOverCapOnlyPastKMaxListItems) {
  // Right at the cap: not over.
  {
    StringSink sink;
    JsonListWriter writer(sink);
    std::vector<std::vector<uint8_t>> items;
    for (int i = 0; i < 255; ++i) items.push_back(item(static_cast<uint16_t>(i), 0, "x"));
    std::vector<std::vector<uint8_t>> groups;
    groups.push_back(groupOf(1, "G1", items));
    // 255 + 255 + (kMaxListItems - 510) = kMaxListItems exactly.
    std::vector<std::vector<uint8_t>> items2;
    for (int i = 0; i < 255; ++i) items2.push_back(item(static_cast<uint16_t>(1000 + i), 0, "x"));
    groups.push_back(groupOf(2, "G2", items2));
    std::vector<std::vector<uint8_t>> items3;
    for (size_t i = 0; i < companionble::kMaxListItems - 510; ++i) {
      items3.push_back(item(static_cast<uint16_t>(2000 + i), 0, "x"));
    }
    groups.push_back(groupOf(3, "G3", items3));
    const auto wire = doc(1, {listOf(1, "L", groups)});
    driveDocument(wire, writer);
    ASSERT_TRUE(writer.ok());
    EXPECT_FALSE(writer.overCap());
  }
  // One item past the cap: over.
  {
    StringSink sink;
    JsonListWriter writer(sink);
    std::vector<std::vector<uint8_t>> items;
    for (int i = 0; i < 255; ++i) items.push_back(item(static_cast<uint16_t>(i), 0, "x"));
    std::vector<std::vector<uint8_t>> groups;
    groups.push_back(groupOf(1, "G1", items));
    std::vector<std::vector<uint8_t>> items2;
    for (int i = 0; i < 255; ++i) items2.push_back(item(static_cast<uint16_t>(1000 + i), 0, "x"));
    groups.push_back(groupOf(2, "G2", items2));
    std::vector<std::vector<uint8_t>> items3;
    for (size_t i = 0; i < companionble::kMaxListItems - 509; ++i) {
      items3.push_back(item(static_cast<uint16_t>(2000 + i), 0, "x"));
    }
    groups.push_back(groupOf(3, "G3", items3));
    const auto wire = doc(1, {listOf(1, "L", groups)});
    driveDocument(wire, writer);
    ASSERT_TRUE(writer.ok());
    EXPECT_TRUE(writer.overCap());
  }
}

}  // namespace
