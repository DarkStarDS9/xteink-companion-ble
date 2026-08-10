#pragma once

#include <cstddef>
#include <cstdint>

// The ToDo List document wire codec (kFieldListDoc, 0x08) -- see
// docs/companion-todo-list-design.md sections 2 and 4. Mirrors
// CompanionUiDeclaration.h's charter exactly, and for the same reason: this
// must compile in the host test harness (test/companion_todo_document/), so
// it may depend on nothing but <cstdint>/<cstddef> (+ CompanionBle.h's field
// id if a caller wants it, which this header itself does not need). No
// ArduinoJson, no HalStorage, no Arduino types, ever -- CompanionPeerStore.cpp
// pulls those in and therefore cannot be host-built at all; nothing in here
// may acquire a dependency that changes that.
//
// No allocation, no state, no clock: parseDocument() below walks a
// caller-supplied buffer in place and reports each element to a
// caller-supplied Visitor. The buffer itself is a transient heap allocation
// owned by the one call site that reassembles a push
// (docs/companion-todo-list-design.md section 4: makeUniqueNoThrow'd for the
// duration of one kFieldListDoc push, written straight through to
// lists.json, freed immediately after) -- this parser never allocates one of
// its own, and never needs to: it only ever reads the caller's buffer.
namespace companiontodo {

// The wire layout, as pushed on kFieldListDoc -- the single source of truth
// for this format, exactly as CompanionUiDeclaration.h documents the UI
// declaration's layout in one place rather than letting each consumer infer
// it from a call site.
//
//   u32  revision                       (little-endian)
//   u8   list count L
//   L x {
//     u16 listId                        (little-endian)
//     u8  titleLen, title[titleLen]     (UTF-8, not NUL-terminated)
//     u8  group count G
//     G x {
//       u16 groupId                     (little-endian)
//       u8  labelLen, label[labelLen]   (empty label == the list's
//                                        "ungrouped" bucket, design doc S2)
//       u8  item count I
//       I x {
//         u16 itemId                    (little-endian)
//         u8  checked                   (0 or 1 ONLY; any other value is
//                                        malformed)
//         u8  textLen, text[textLen]
//       }
//     }
//   }
//
// ENDIANNESS: little-endian, matching the convention already established on
// this wire for every other multi-byte field -- the START packet's total
// payload length (u32) and the image CHUNK sequence number (u16)
// (docs/companion-display-protocol.md), and the button-hold duration field
// (u16). This format follows that existing convention rather than picking its
// own.
//
// WHY IDS ARE u16, NOT u8: kMaxListDocLen (CompanionBle.h) is 16 KB, which
// comfortably admits on the order of 500 short items -- a document that size
// can legally hold more than 255 of them, so a u8 id would silently overflow
// inside an otherwise-legal document. u16 leaves headroom no realistic
// document approaches, at a one-byte-per-id cost that is nothing against a
// 16 KB cap. Ids are device-opaque throughout (design doc S2): the firmware
// only ever compares and echoes them, it never generates or interprets one.

// Smallest legal document: a 4-byte revision plus a list count of zero.
inline constexpr size_t kMinDocLen = 5;

enum class ParseResult : uint8_t {
  Ok,         // structurally sound; every Visitor callback below fired
  Malformed,  // truncated, over-long, or an entry that runs off the end
};

// Receives one callback per element of the document, in wire order: the
// document header, then each list in turn, each list's groups in turn, each
// group's items in turn, closed by matching *End callbacks. Default bodies do
// nothing, so a caller only overrides what it needs -- a renderer for one
// list overrides onListStart to skip lists it does not care about, an
// ingest-to-JSON walk overrides everything.
//
// Every string is a pointer *into the caller's own buffer* plus a length --
// never NUL-terminated, never copied here. Both known consumers work this way
// already: a JSON writer copies bytes out itself as it goes, and a renderer
// only needs to read glyphs for the rows currently on screen. Materializing
// std::string or fixed-size structs in this parser would be resident cost
// neither consumer asked for, and would violate this file's no-allocation
// charter above.
//
// Callbacks always run in full for a document that parses Ok -- there is no
// early-stop signal. At <=16 KB and on the order of a few hundred items
// (kMaxListDocLen), a full walk is cheap enough that a windowed renderer can
// simply ignore callbacks outside its visible window rather than the parser
// needing to support a partial walk; keeping the parser itself
// all-or-nothing is what lets it validate the *whole* document before ever
// calling out, matching CompanionUiDeclaration's validate-before-trust
// posture. See parseDocument()'s comment for the one exception: a Malformed
// verdict may follow some callbacks having already fired, which the caller is
// expected to discard, exactly as CompanionUiDeclaration::walkBody()'s
// partially-written DeclarationInfo is discarded on failure.
class Visitor {
 public:
  virtual ~Visitor() = default;

  virtual void onDocument(uint32_t revision) { (void)revision; }
  virtual void onListStart(uint16_t listId, const char* title, uint8_t titleLen) {
    (void)listId;
    (void)title;
    (void)titleLen;
  }
  virtual void onGroupStart(uint16_t groupId, const char* label, uint8_t labelLen) {
    (void)groupId;
    (void)label;
    (void)labelLen;
  }
  virtual void onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) {
    (void)itemId;
    (void)checked;
    (void)text;
    (void)textLen;
  }
  virtual void onGroupEnd(uint16_t groupId) { (void)groupId; }
  virtual void onListEnd(uint16_t listId) { (void)listId; }
};

// Validates `data[0..len)` against the layout above and visits it in a single
// forward pass, calling `visitor`'s methods as each element is confirmed
// structurally sound. On Malformed, some callbacks for earlier, valid-looking
// elements may already have fired before the bad byte was reached -- the same
// "caller discards partial output on failure" contract
// CompanionUiDeclaration::walkBody() uses, stated here because a Visitor is
// caller-owned state rather than an out-param that is easy to see is
// unwritten.
//
// This function enforces no length cap of its own; callers are expected to
// have already bounded `len` (CompanionBle.cpp's kFieldListDoc handling
// enforces kMaxListDocLen before this is ever called), the same division of
// labour CompanionUiDeclaration has with kMaxUiDeclarationLen.
ParseResult parseDocument(const uint8_t* data, size_t len, Visitor& visitor);

}  // namespace companiontodo
