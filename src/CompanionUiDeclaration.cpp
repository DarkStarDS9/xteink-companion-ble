#include "CompanionUiDeclaration.h"

namespace companionui {

bool isKnownShape(uint8_t raw) {
  return raw == static_cast<uint8_t>(companionble::ContentShape::Text) ||
         raw == static_cast<uint8_t>(companionble::ContentShape::Image) ||
         raw == static_cast<uint8_t>(companionble::ContentShape::List);
}

namespace {

// Walks buttons, the optional tag section, and the optional trailing bytes,
// past the mandatory shape byte (button count at offset 1). Returns false on
// any truncation or overrun; `info` is left partially written on failure,
// which is fine because the caller discards it in that case.
bool walkBody(const uint8_t* body, size_t len, DeclarationInfo* info) {
  const uint8_t buttonCount = body[1];
  size_t offset = kBodyFirstButtonOffset;
  for (uint8_t i = 0; i < buttonCount; ++i) {
    if (offset + 3 > len) return false;
    offset += 3 + body[offset + 2];
    if (offset > len) return false;
  }
  info->buttonCount = buttonCount;

  // The tag section is optional: an app with no tags may simply stop after its
  // buttons rather than append a zero byte.
  if (offset != len) {
    info->hasTagSection = true;
    const uint8_t tagCount = body[offset++];
    info->tagCount = tagCount;
    for (uint8_t i = 0; i < tagCount; ++i) {
      if (offset + 2 > len) return false;
      offset += 2 + body[offset + 1];
      if (offset > len) return false;
    }
  }

  // Up to two more trailing bytes may follow the tag section: tag render
  // style, then capabilities. Both are optional and independently absent —
  // "ran out of buffer" is how a decoder tells absent from present, so
  // anything beyond two extra bytes here is not a declaration this version
  // knows how to produce.
  if (len - offset > 2) return false;
  if (offset < len) {
    info->hasTagRenderStyle = true;
    // Reported verbatim, not clamped: only the shape is a closed set here. A
    // style the device does not know is cosmetic and the render path already
    // falls back to Bordered (CompanionModeActivity::loadUiDeclaration()), so
    // refusing the whole declaration over it would be a regression.
    info->tagRenderStyle = static_cast<companionble::TagRenderStyle>(body[offset]);
    ++offset;
  }
  if (offset < len) {
    info->hasCapabilities = true;
    info->capabilities = body[offset];
    ++offset;
  }
  return true;
}

}  // namespace

// The declaration is validated at store time rather than at render time so a
// malformed push is refused on ASSET_ACK while the app can still do something
// about it — a declaration that half-parsed would draw nonsense hints with no
// way to find out why. See the header for the byte layout and for why the
// mandatory shape byte sits in front of the optional trailing block.
//
// A stale (pre-v12) client -- one that never wrote the shape byte at all --
// cannot reach this: HELLO's protocolVersion field (v12,
// docs/companion-display-protocol.md's Session characteristic section) refuses
// it before it has a session to push a declaration on. So unlike the v12
// migration window, NoShape here is never an inference about the client's
// age -- it is a compliant client's declaration that parses structurally but
// carries a shape byte with no known value (0x00, or above List).
ParseResult parseBody(const uint8_t* body, size_t len, DeclarationInfo* out) {
  if (body == nullptr || len < kMinBodyLen) return ParseResult::Malformed;

  const uint8_t rawShape = body[0];
  DeclarationInfo info;
  if (!walkBody(body, len, &info)) return ParseResult::Malformed;

  if (!isKnownShape(rawShape)) return ParseResult::NoShape;
  info.shape = static_cast<companionble::ContentShape>(rawShape);
  if (out != nullptr) *out = info;
  return ParseResult::Ok;
}

ParseResult parseAsset(const uint8_t* data, size_t len, DeclarationInfo* out) {
  if (data == nullptr || len < kDigestLen) return ParseResult::Malformed;
  return parseBody(data + kDigestLen, len - kDigestLen, out);
}

bool fieldMatchesShape(uint8_t field, uint8_t declaredShape) {
  switch (static_cast<companionble::ContentShape>(declaredShape)) {
    case companionble::ContentShape::Text:
      return field == companionble::kFieldTitle || field == companionble::kFieldBody ||
             field == companionble::kFieldContentId || field == companionble::kFieldTagState;
    case companionble::ContentShape::Image:
      // Tag state is an overlay, not content: a visible tag renders as a chip
      // over the print, and a tag pushed *with* the image is drawn when the
      // image is drawn (docs/companion-display-protocol.md, "Tags are drawn
      // over an image"). Refusing 0x07 here would un-design that atomic
      // image+tag push and reinstate the silent no-op that section says is
      // gone, so the overlay field is permitted under both content shapes.
      return field == companionble::kFieldImage || field == companionble::kFieldTagState;
    case companionble::ContentShape::List:
      // No list content field exists on the wire yet (see
      // docs/companion-todo-list-design.md), so a LIST peer pushing any content
      // field today is pushing something it could not render.
      return false;
  }
  // Not reachable for a shape that came out of an Ok parse, which is the only
  // way a shape is ever cached. Refuse rather than admit: an unknown shape is
  // precisely the "nobody knows what this peer pushes" state the declaration
  // exists to make impossible.
  return false;
}

}  // namespace companionui
