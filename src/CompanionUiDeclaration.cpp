#include "CompanionUiDeclaration.h"

namespace companionui {

bool isKnownShape(uint8_t raw) {
  return raw == static_cast<uint8_t>(companionble::ContentShape::Text) ||
         raw == static_cast<uint8_t>(companionble::ContentShape::Image) ||
         raw == static_cast<uint8_t>(companionble::ContentShape::List);
}

// The declaration is validated at store time rather than at render time so a
// malformed push is refused on ASSET_ACK while the app can still do something
// about it — a declaration that half-parsed would draw nonsense hints with no
// way to find out why. See the header for the byte layout and for why the
// mandatory shape byte sits in front of the optional trailing block.
ParseResult parseBody(const uint8_t* body, size_t len, DeclarationInfo* out) {
  if (body == nullptr || len < kMinBodyLen) return ParseResult::Malformed;

  // Structure is checked before the shape value, so a genuinely corrupt buffer
  // that happens to start with 0x02 is still reported Malformed rather than
  // silently accepted; the shape verdict is held until the walk succeeds.
  const uint8_t rawShape = body[0];

  const uint8_t buttonCount = body[1];
  size_t offset = kBodyFirstButtonOffset;
  for (uint8_t i = 0; i < buttonCount; ++i) {
    if (offset + 3 > len) return ParseResult::Malformed;
    offset += 3 + body[offset + 2];
    if (offset > len) return ParseResult::Malformed;
  }

  DeclarationInfo info;
  info.buttonCount = buttonCount;

  // The tag section is optional: an app with no tags may simply stop after its
  // buttons rather than append a zero byte.
  if (offset != len) {
    info.hasTagSection = true;
    const uint8_t tagCount = body[offset++];
    info.tagCount = tagCount;
    for (uint8_t i = 0; i < tagCount; ++i) {
      if (offset + 2 > len) return ParseResult::Malformed;
      offset += 2 + body[offset + 1];
      if (offset > len) return ParseResult::Malformed;
    }
  }

  // Up to two more trailing bytes may follow the tag section: tag render
  // style, then capabilities. Both are optional and independently absent —
  // "ran out of buffer" is how a decoder tells absent from present, so
  // anything beyond two extra bytes here is not a declaration this version
  // knows how to produce.
  if (len - offset > 2) return ParseResult::Malformed;
  if (offset < len) {
    info.hasTagRenderStyle = true;
    // Reported verbatim, not clamped: only the shape is a closed set here. A
    // style the device does not know is cosmetic and the render path already
    // falls back to Bordered (CompanionModeActivity::loadUiDeclaration()), so
    // refusing the whole declaration over it would be a regression.
    info.tagRenderStyle = static_cast<companionble::TagRenderStyle>(body[offset]);
    ++offset;
  }
  if (offset < len) {
    info.hasCapabilities = true;
    info.capabilities = body[offset];
    ++offset;
  }

  // Shape last, so that "your declaration is missing its shape byte" is only
  // ever said about a declaration that is otherwise sound — the whole reason
  // AssetStoreResult::RejectedNoShape exists as a code distinct from
  // RejectedFormat (docs/companion-declared-shape-design.md section 3).
  if (!isKnownShape(rawShape)) return ParseResult::NoShape;
  info.shape = static_cast<companionble::ContentShape>(rawShape);

  if (out != nullptr) *out = info;
  return ParseResult::Ok;
}

ParseResult parseAsset(const uint8_t* data, size_t len, DeclarationInfo* out) {
  if (data == nullptr || len < kDigestLen) return ParseResult::Malformed;
  return parseBody(data + kDigestLen, len - kDigestLen, out);
}

}  // namespace companionui
