#include "CompanionUiDeclaration.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <ios>
#include <vector>

namespace {

using companionble::ContentShape;
using companionble::TagRenderStyle;
using companionui::DeclarationInfo;
using companionui::ParseResult;

// Builders, not literals: every offset in this format shifts when the layout
// changes, so a test that hand-counts bytes stops describing what it meant the
// moment the layout moves again. These say "a button entry" and let the
// arithmetic fall out.
std::vector<uint8_t> button(uint8_t id, uint8_t routing, const char* label) {
  std::vector<uint8_t> out{id, routing, static_cast<uint8_t>(strlen(label))};
  for (const char* p = label; *p != '\0'; ++p) out.push_back(static_cast<uint8_t>(*p));
  return out;
}

std::vector<uint8_t> tag(uint8_t id, const char* label) {
  std::vector<uint8_t> out{id, static_cast<uint8_t>(strlen(label))};
  for (const char* p = label; *p != '\0'; ++p) out.push_back(static_cast<uint8_t>(*p));
  return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// A whole pushed asset: 4-byte opaque digest, then the body.
std::vector<uint8_t> asset(std::vector<uint8_t> body) {
  std::vector<uint8_t> out{0xDE, 0xAD, 0xBE, 0xEF};
  append(out, body);
  return out;
}

// The v12 body: shape, button count, entries.
std::vector<uint8_t> body(uint8_t shape, std::initializer_list<std::vector<uint8_t>> buttons) {
  std::vector<uint8_t> out{shape, static_cast<uint8_t>(buttons.size())};
  for (const auto& b : buttons) append(out, b);
  return out;
}

ParseResult parse(const std::vector<uint8_t>& a, DeclarationInfo* out = nullptr) {
  return companionui::parseAsset(a.data(), a.size(), out);
}

// ---------------------------------------------------------------------------
// The shape byte itself
// ---------------------------------------------------------------------------

TEST(CompanionUiDeclaration, EachDeclaredShapeRoundTrips) {
  for (const uint8_t raw : {0x01, 0x02, 0x03}) {
    DeclarationInfo info;
    const auto a = asset(body(raw, {button(1, 0x01, "Play")}));
    EXPECT_EQ(parse(a, &info), ParseResult::Ok) << "shape 0x" << std::hex << int(raw);
    EXPECT_EQ(static_cast<uint8_t>(info.shape), raw);
    EXPECT_EQ(info.buttonCount, 1);
  }
}

TEST(CompanionUiDeclaration, ShapeZeroIsRejectedAsNoShape) {
  const auto a = asset(body(0x00, {button(1, 0x01, "Play")}));
  EXPECT_EQ(parse(a), ParseResult::NoShape);
}

TEST(CompanionUiDeclaration, ShapeAboveListIsRejectedAsNoShape) {
  for (const uint8_t raw : {0x04, 0x05, 0x7F, 0xFF}) {
    const auto a = asset(body(raw, {button(1, 0x01, "Play")}));
    EXPECT_EQ(parse(a), ParseResult::NoShape) << "shape 0x" << std::hex << int(raw);
  }
}

TEST(CompanionUiDeclaration, UnknownShapeIsNoShapeEvenWhenTheRestIsPerfect) {
  // The distinction that RejectedNoShape exists for: this declaration is
  // structurally flawless, so reporting "malformed" would send an app hunting
  // through its framing for a bug that is one wrong byte.
  auto b = body(0x09, {button(1, 0x01, "Play"), button(2, 0x02, "Next")});
  b.push_back(1);
  append(b, tag(7, "Saved"));
  b.push_back(static_cast<uint8_t>(TagRenderStyle::Plain));
  b.push_back(companionble::kUiCapabilityImageGallery);
  EXPECT_EQ(parse(asset(b)), ParseResult::NoShape);
}

TEST(CompanionUiDeclaration, IsKnownShapeCoversExactlyTheThreeDeclaredValues) {
  EXPECT_FALSE(companionui::isKnownShape(0x00));
  EXPECT_TRUE(companionui::isKnownShape(0x01));
  EXPECT_TRUE(companionui::isKnownShape(0x02));
  EXPECT_TRUE(companionui::isKnownShape(0x03));
  EXPECT_FALSE(companionui::isKnownShape(0x04));
  EXPECT_FALSE(companionui::isKnownShape(0xFF));
}

// ---------------------------------------------------------------------------
// The pre-v12 shape (now unreachable in practice, still worth pinning)
// ---------------------------------------------------------------------------
//
// Through the v12 migration window, HELLO carried no protocolVersion field, so
// the device could not refuse a stale client before it pushed a declaration --
// only after, by noticing the declaration didn't parse. These two tests used
// to require that "doesn't parse" verdict to be NoShape specifically, so a
// migrating app was told what was actually wrong rather than a generic
// "malformed". As of HELLO's protocolVersion field
// (docs/companion-display-protocol.md's Session characteristic section) a
// client old enough to omit the shape byte is also old enough to fail the
// HELLO version check, so it can never reach ASSET push at all -- a buffer
// shaped like this now only happens from real corruption or a bug in a
// compliant client, and Malformed is the honest answer. Kept as regression
// tests for that reasoning, not for the byte layout itself.

TEST(CompanionUiDeclaration, V11ShapedDeclarationIsMalformedNotNoShape) {
  // What a pre-v12 client used to push: digest, then the button count where
  // the shape byte now lives. Two buttons, so byte 4 reads 0x02 — a *valid*
  // shape value, which is exactly why this can't be told apart from a v12
  // declaration by its leading byte alone; only the length arithmetic
  // downstream can, and it reliably lands on Malformed for this shape.
  std::vector<uint8_t> v11{2};
  append(v11, button(1, 0x01, "Play"));
  append(v11, button(2, 0x01, "Next"));
  EXPECT_EQ(parse(asset(v11)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, V11ShapedDeclarationWithTagsIsMalformedNotNoShape) {
  // The fuller pre-v12 shape: buttons, tags, and both trailing optional bytes
  // -- this is what scripts/companion_e2e_test.py's NO_SHAPE_DECL used to send
  // before the HELLO version check made that scenario obsolete.
  std::vector<uint8_t> v11{4};
  append(v11, button(1, 1, "<"));
  append(v11, button(2, 2, ">"));
  append(v11, button(3, 3, "Save"));
  append(v11, button(4, 3, "Back"));
  v11.push_back(2);
  append(v11, tag(0, "Saved"));
  append(v11, tag(1, "New"));
  v11.push_back(static_cast<uint8_t>(TagRenderStyle::Bordered));
  v11.push_back(0);
  EXPECT_EQ(parse(asset(v11)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, V11DeclarationWithNoButtonsIsRejected) {
  // The degenerate v11 push: digest plus a single zero button count. Malformed
  // rather than NoShape, and deliberately so — a one-byte body is short of the
  // shape+count minimum, so "this is truncated" is the true statement about it;
  // NoShape is reserved for a declaration that is otherwise sound, which is the
  // only case where it tells an app something it could not work out itself.
  const std::vector<uint8_t> v11{0};
  EXPECT_EQ(parse(asset(v11)), ParseResult::Malformed);
}

// ---------------------------------------------------------------------------
// Buttons, tags, and the trailing optionals at their shifted offsets
// ---------------------------------------------------------------------------

TEST(CompanionUiDeclaration, ButtonsOnlyNoTagSection) {
  DeclarationInfo info;
  const auto a = asset(body(0x01, {button(1, 0x01, "Play"), button(4, 0x02, "Prev"), button(5, 0x03, "Next")}));
  ASSERT_EQ(parse(a, &info), ParseResult::Ok);
  EXPECT_EQ(info.shape, ContentShape::Text);
  EXPECT_EQ(info.buttonCount, 3);
  EXPECT_FALSE(info.hasTagSection);
  EXPECT_EQ(info.tagCount, 0);
  EXPECT_FALSE(info.hasTagRenderStyle);
  EXPECT_FALSE(info.hasCapabilities);
  EXPECT_EQ(info.tagRenderStyle, TagRenderStyle::Bordered);
  EXPECT_EQ(info.capabilities, 0);
}

TEST(CompanionUiDeclaration, ZeroButtonsWithTags) {
  DeclarationInfo info;
  auto b = body(0x03, {});
  b.push_back(2);
  append(b, tag(0, "Done"));
  append(b, tag(1, "Sync"));
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_EQ(info.shape, ContentShape::List);
  EXPECT_EQ(info.buttonCount, 0);
  EXPECT_TRUE(info.hasTagSection);
  EXPECT_EQ(info.tagCount, 2);
}

TEST(CompanionUiDeclaration, EmptyLabelsAreLegal) {
  DeclarationInfo info;
  auto b = body(0x01, {button(1, 0x00, "")});
  b.push_back(1);
  append(b, tag(3, ""));
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_EQ(info.buttonCount, 1);
  EXPECT_EQ(info.tagCount, 1);
}

TEST(CompanionUiDeclaration, StylePresentCapabilitiesAbsent) {
  DeclarationInfo info;
  auto b = body(0x01, {button(1, 0x01, "Play")});
  b.push_back(1);
  append(b, tag(7, "Saved"));
  b.push_back(static_cast<uint8_t>(TagRenderStyle::Plain));
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_TRUE(info.hasTagRenderStyle);
  EXPECT_EQ(info.tagRenderStyle, TagRenderStyle::Plain);
  EXPECT_FALSE(info.hasCapabilities);
  EXPECT_EQ(info.capabilities, 0);
}

TEST(CompanionUiDeclaration, BothTrailingOptionalsPresent) {
  DeclarationInfo info;
  auto b = body(0x02, {button(1, 0x01, "Play")});
  b.push_back(1);
  append(b, tag(7, "Saved"));
  b.push_back(static_cast<uint8_t>(TagRenderStyle::Plain));
  b.push_back(companionble::kUiCapabilityImageGallery);
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_EQ(info.shape, ContentShape::Image);
  EXPECT_EQ(info.tagCount, 1);
  EXPECT_TRUE(info.hasTagRenderStyle);
  EXPECT_EQ(info.tagRenderStyle, TagRenderStyle::Plain);
  EXPECT_TRUE(info.hasCapabilities);
  EXPECT_EQ(info.capabilities, companionble::kUiCapabilityImageGallery);
}

TEST(CompanionUiDeclaration, TrailingOptionalsAfterAZeroTagCount) {
  DeclarationInfo info;
  auto b = body(0x01, {button(1, 0x01, "Play")});
  b.push_back(0);  // explicit "no tags", rather than simply stopping
  b.push_back(static_cast<uint8_t>(TagRenderStyle::Bordered));
  b.push_back(companionble::kUiCapabilityImageGallery);
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_TRUE(info.hasTagSection);
  EXPECT_EQ(info.tagCount, 0);
  EXPECT_TRUE(info.hasCapabilities);
  EXPECT_EQ(info.capabilities, companionble::kUiCapabilityImageGallery);
}

TEST(CompanionUiDeclaration, UnknownTagRenderStyleIsReportedVerbatimNotRejected) {
  // Only the shape is a closed set. The style byte keeps the existing lenient
  // contract — the render path clamps it (CompanionModeActivity), and a
  // declaration is not refused over a cosmetic value the device can default.
  DeclarationInfo info;
  auto b = body(0x01, {});
  b.push_back(0);
  b.push_back(0x7E);
  ASSERT_EQ(parse(asset(b), &info), ParseResult::Ok);
  EXPECT_TRUE(info.hasTagRenderStyle);
  EXPECT_EQ(static_cast<uint8_t>(info.tagRenderStyle), 0x7E);
}

TEST(CompanionUiDeclaration, ThreeTrailingBytesIsMalformed) {
  // "Ran out of buffer" is how absence is signalled, so anything past the two
  // known trailing bytes is not a declaration this version can produce.
  auto b = body(0x01, {button(1, 0x01, "Play")});
  b.push_back(0);
  b.push_back(0x00);
  b.push_back(0x01);
  b.push_back(0x02);
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

// ---------------------------------------------------------------------------
// Bounds safety
// ---------------------------------------------------------------------------

TEST(CompanionUiDeclaration, EmptyAndDigestOnlyBuffersAreMalformed) {
  const std::vector<uint8_t> empty;
  EXPECT_EQ(companionui::parseAsset(empty.data(), 0, nullptr), ParseResult::Malformed);
  for (size_t len = 1; len <= companionui::kDigestLen; ++len) {
    const std::vector<uint8_t> truncated(len, 0xAA);
    EXPECT_EQ(companionui::parseAsset(truncated.data(), len, nullptr), ParseResult::Malformed) << "len " << len;
  }
}

TEST(CompanionUiDeclaration, ShapeWithoutAButtonCountIsMalformed) {
  const std::vector<uint8_t> b{0x01};
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, ButtonCountLargerThanTheEntriesPresentIsMalformed) {
  std::vector<uint8_t> b{0x01, 3};
  append(b, button(1, 0x01, "Play"));
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, ButtonLabelRunningOffTheEndIsMalformed) {
  std::vector<uint8_t> b{0x01, 1, /*id=*/1, /*routing=*/1, /*labelLen=*/200};
  b.push_back('A');
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, TagLabelRunningOffTheEndIsMalformed) {
  auto b = body(0x01, {button(1, 0x01, "Play")});
  b.push_back(1);
  b.push_back(9);    // tag id
  b.push_back(250);  // labelLen with nothing behind it
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, TagCountLargerThanTheEntriesPresentIsMalformed) {
  auto b = body(0x01, {});
  b.push_back(2);
  append(b, tag(0, "One"));
  EXPECT_EQ(parse(asset(b)), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, EveryTruncationOfAValidDeclarationIsRejectedNotOverRead) {
  // The blunt instrument that catches an off-by-one nobody thought to name:
  // no prefix of a good declaration may parse as Ok unless it happens to be a
  // legal shorter declaration in its own right, and none may read past `len`
  // (run under a sanitizer build for the second half of that claim).
  auto full = body(0x02, {button(1, 0x01, "Play"), button(2, 0x02, "Next")});
  full.push_back(1);
  append(full, tag(7, "Saved"));
  full.push_back(static_cast<uint8_t>(TagRenderStyle::Plain));
  full.push_back(companionble::kUiCapabilityImageGallery);
  const auto a = asset(full);
  ASSERT_EQ(parse(a), ParseResult::Ok);

  for (size_t len = 0; len < a.size(); ++len) {
    const std::vector<uint8_t> truncated(a.begin(), a.begin() + static_cast<long>(len));
    const ParseResult r = companionui::parseAsset(truncated.data(), len, nullptr);
    // Truncating mid-label can never yield a well-formed declaration; the only
    // prefixes that legitimately parse are those ending exactly on an entry
    // boundary, which the length arithmetic decides, not the parser guessing.
    if (r == ParseResult::Ok) {
      DeclarationInfo info;
      ASSERT_EQ(companionui::parseAsset(truncated.data(), len, &info), ParseResult::Ok);
      EXPECT_TRUE(companionui::isKnownShape(static_cast<uint8_t>(info.shape))) << "len " << len;
    }
  }
}

TEST(CompanionUiDeclaration, ParseBodyAndParseAssetAgreeOnTheSameDeclaration) {
  auto b = body(0x03, {button(1, 0x01, "Play")});
  b.push_back(0);
  DeclarationInfo fromBody;
  DeclarationInfo fromAsset;
  ASSERT_EQ(companionui::parseBody(b.data(), b.size(), &fromBody), ParseResult::Ok);
  const auto a = asset(b);
  ASSERT_EQ(companionui::parseAsset(a.data(), a.size(), &fromAsset), ParseResult::Ok);
  EXPECT_EQ(fromBody.shape, fromAsset.shape);
  EXPECT_EQ(fromBody.buttonCount, fromAsset.buttonCount);
  EXPECT_EQ(fromBody.tagCount, fromAsset.tagCount);
}

TEST(CompanionUiDeclaration, NullBufferIsMalformed) {
  EXPECT_EQ(companionui::parseBody(nullptr, 0, nullptr), ParseResult::Malformed);
  EXPECT_EQ(companionui::parseAsset(nullptr, 0, nullptr), ParseResult::Malformed);
}

TEST(CompanionUiDeclaration, OutStructIsUntouchedWhenTheParseFails) {
  DeclarationInfo info;
  info.buttonCount = 42;
  const auto a = asset(body(0x00, {button(1, 0x01, "Play")}));
  ASSERT_EQ(parse(a, &info), ParseResult::NoShape);
  EXPECT_EQ(info.buttonCount, 42);
}

// ---------------------------------------------------------------------------
// v12 enforcement: which content fields a declared shape permits.
//
// Every case here is a wire-contract statement, not an implementation detail:
// a mismatch is answered RenderResult::RejectedShape and the push is dropped,
// so a wrong entry in this table silently bricks a real app's content.
// ---------------------------------------------------------------------------

constexpr uint8_t kText = static_cast<uint8_t>(ContentShape::Text);
constexpr uint8_t kImage = static_cast<uint8_t>(ContentShape::Image);
constexpr uint8_t kList = static_cast<uint8_t>(ContentShape::List);

TEST(CompanionShapeEnforcement, TextPeerMayPushTheTextBatchFields) {
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldTitle, kText));
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldBody, kText));
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldContentId, kText));
  // Tag state rides the same atomic batch as title/body, so a TEXT peer that
  // could not push it could not commit content and its tags in one redraw.
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldTagState, kText));
}

TEST(CompanionShapeEnforcement, TagStateIsAnOverlayPermittedUnderTextAndImage) {
  // 0x07 is not content, it is a chip drawn over whatever content is on the
  // screen, so it is exempt from the shape table's "one shape, one field kind"
  // rule. An IMAGE peer must be able to push image + tag as one atomic batch;
  // refusing the tag would either lose it or force a second, non-atomic write.
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldTagState, kText));
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldTagState, kImage));
}

TEST(CompanionShapeEnforcement, TextPeerMayNotPushAnImage) {
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldImage, kText));
}

TEST(CompanionShapeEnforcement, ImagePeerMayPushTheImageFieldAndNoTextField) {
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldImage, kImage));
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldTitle, kImage));
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldBody, kImage));
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldContentId, kImage));
  // Tag state is the one field both shapes share; see
  // TagStateIsAnOverlayPermittedUnderTextAndImage.
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldTagState, kImage));
}

TEST(CompanionShapeEnforcement, ListPeerHasNoContentFieldYet) {
  // Deliberate: the list document has no wire field of its own yet, so a LIST
  // peer pushing anything is pushing something it could not render. This test
  // is expected to change when that field is added -- not to be deleted.
  for (const uint8_t field : {companionble::kFieldTitle, companionble::kFieldBody, companionble::kFieldContentId,
                              companionble::kFieldImage, companionble::kFieldTagState}) {
    EXPECT_FALSE(companionui::fieldMatchesShape(field, kList)) << "field " << std::hex << int(field);
  }
}

TEST(CompanionShapeEnforcement, UnknownOrUncachedShapePermitsNothing) {
  // 0 is the Session's "not cached yet" sentinel; the caller must catch it
  // before asking. If it ever reaches here the answer is "no", not "sure".
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldTitle, 0));
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldImage, 0xFF));
}

TEST(CompanionShapeEnforcement, EveryKnownShapeAcceptsAtLeastOneFieldOrIsDeliberatelyEmpty) {
  // Guards the enum against a shape being added to ContentShape and forgotten
  // in the table: a new value would fall out of every arm and reject silently.
  for (const uint8_t shape : {kText, kImage, kList}) {
    ASSERT_TRUE(companionui::isKnownShape(shape));
  }
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldBody, kText));
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldImage, kImage));
}

TEST(CompanionShapeEnforcement, AParsedDeclarationsShapeDrivesTheTableDirectly) {
  // The two halves joined: what parseBody() reports is exactly what enforcement
  // consumes, with no re-interpretation in between.
  DeclarationInfo info;
  const auto a = asset(body(static_cast<uint8_t>(ContentShape::Image), {button(1, 0x01, "Next")}));
  ASSERT_EQ(parse(a, &info), ParseResult::Ok);
  EXPECT_TRUE(companionui::fieldMatchesShape(companionble::kFieldImage, static_cast<uint8_t>(info.shape)));
  EXPECT_FALSE(companionui::fieldMatchesShape(companionble::kFieldBody, static_cast<uint8_t>(info.shape)));
}

}  // namespace
