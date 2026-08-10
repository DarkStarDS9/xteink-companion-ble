#include "CompanionTodoDocument.h"

namespace companiontodo {

namespace {

// Reads a u16 little-endian value at `data[offset]`. Caller has already
// checked `offset + 2 <= len`.
uint16_t readU16LE(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

// Reads a u32 little-endian value at `data[offset]`. Caller has already
// checked `offset + 4 <= len`.
uint32_t readU32LE(const uint8_t* data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

// Reads a length-prefixed string (1-byte length, then that many bytes) at
// `data[offset]`, advancing `offset` past it. Returns false -- leaving
// `offset` unspecified -- if the length prefix or the string body would run
// past `len`.
bool readString(const uint8_t* data, size_t len, size_t* offset, const char** out, uint8_t* outLen) {
  if (*offset + 1 > len) return false;
  const uint8_t strLen = data[*offset];
  ++*offset;
  if (*offset + strLen > len) return false;
  *out = reinterpret_cast<const char*>(data + *offset);
  *outLen = strLen;
  *offset += strLen;
  return true;
}

}  // namespace

ParseResult parseDocument(const uint8_t* data, size_t len, Visitor& visitor) {
  if (data == nullptr || len < kMinDocLen) return ParseResult::Malformed;

  size_t offset = 0;
  const uint32_t revision = readU32LE(data, offset);
  offset += 4;

  const uint8_t listCount = data[offset];
  ++offset;
  visitor.onDocument(revision);

  for (uint8_t li = 0; li < listCount; ++li) {
    if (offset + 2 > len) return ParseResult::Malformed;
    const uint16_t listId = readU16LE(data, offset);
    offset += 2;

    const char* title = nullptr;
    uint8_t titleLen = 0;
    if (!readString(data, len, &offset, &title, &titleLen)) return ParseResult::Malformed;

    if (offset + 1 > len) return ParseResult::Malformed;
    const uint8_t groupCount = data[offset];
    ++offset;

    visitor.onListStart(listId, title, titleLen);

    for (uint8_t gi = 0; gi < groupCount; ++gi) {
      if (offset + 2 > len) return ParseResult::Malformed;
      const uint16_t groupId = readU16LE(data, offset);
      offset += 2;

      const char* label = nullptr;
      uint8_t labelLen = 0;
      if (!readString(data, len, &offset, &label, &labelLen)) return ParseResult::Malformed;

      if (offset + 1 > len) return ParseResult::Malformed;
      const uint8_t itemCount = data[offset];
      ++offset;

      visitor.onGroupStart(groupId, label, labelLen);

      for (uint8_t ii = 0; ii < itemCount; ++ii) {
        if (offset + 2 > len) return ParseResult::Malformed;
        const uint16_t itemId = readU16LE(data, offset);
        offset += 2;

        if (offset + 1 > len) return ParseResult::Malformed;
        const uint8_t rawChecked = data[offset];
        if (rawChecked > 1) return ParseResult::Malformed;
        ++offset;

        const char* text = nullptr;
        uint8_t textLen = 0;
        if (!readString(data, len, &offset, &text, &textLen)) return ParseResult::Malformed;

        visitor.onItem(itemId, rawChecked != 0, text, textLen);
      }

      visitor.onGroupEnd(groupId);
    }

    visitor.onListEnd(listId);
  }

  // Nothing may follow the last item: a document that claims fewer lists,
  // groups, or items than the buffer actually holds is malformed the same
  // way CompanionUiDeclaration::walkBody() treats trailing bytes past its own
  // last recognized field as malformed rather than silently ignored.
  if (offset != len) return ParseResult::Malformed;

  return ParseResult::Ok;
}

}  // namespace companiontodo
