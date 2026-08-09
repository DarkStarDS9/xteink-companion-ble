#pragma once

#include <cstddef>
#include <cstdint>

#include "CompanionBle.h"

// The UI-declaration wire codec, extracted out of CompanionPeerStore.cpp's
// uiDeclarationParses()/isImageCapable() so the parse rules can be covered by
// a host-side gtest suite instead of only hardware regressions (see
// test/companion_ui_declaration/), exactly as CompanionBatchModel and
// CompanionConnPolicy were extracted before it. CompanionPeerStore.cpp pulls
// in ArduinoJson, PersistableStore and HalStorage and therefore cannot be
// host-built at all; nothing in here may acquire a dependency that changes
// that. <cstdint>/<cstddef> and CompanionBle.h's enums, and nothing else.
//
// No allocation, no state, no clock: every entry point parses a
// caller-supplied buffer in place and writes plain data into a caller-supplied
// out-struct. The declaration lives in a fixed
// companionpeer::kMaxUiDeclarationLen stack buffer at every call site, and the
// parser must never need one of its own -- see .skills/SKILL.md on heap
// discipline for why that is not negotiable on a no-PSRAM part.
namespace companionui {

// The stored-asset layout, as pushed on kFieldUiDeclaration:
//
//   bytes 0..3   opaque digest
//   byte  4      content shape              (mandatory)
//   byte  5      button entry count N
//   N x { buttonId:1, routing:1, labelLen:1, label[labelLen] }
//   byte         tag entry count M          (optional; absent means zero tags)
//   M x { tagId:1, labelLen:1, label[labelLen] }
//   byte         tag render style           (optional; absent means Bordered)
//   byte         capabilities bitmask       (optional; absent means none)
//
// WHY THE SHAPE BYTE SITS AT OFFSET 4 rather than trailing the optional
// fields, which is the more obvious place for a late addition: the trailing
// fields signal absence by the buffer simply running out. A *mandatory* field
// cannot sit behind optional ones under that convention -- given a declaration
// with a single trailing byte, a decoder has no way to tell a content shape
// from a tag render style. So the one mandatory addition goes in front of the
// optional block, and the trailing-optional convention stays intact for the
// two fields that legitimately use it.
//
// Inserting mid-buffer shifts every following offset by one and is therefore a
// hard break for any client built against the previous layout. That is
// deliberate and is what makes it affordable: see
// docs/companion-declared-shape-design.md section 6 -- every client of this
// protocol is written by this project's author, so there is nobody to strand,
// and a v11 declaration is refused outright rather than tolerated in a
// permissive legacy mode that would have to live in the firmware forever.

// The digest that prefixes a stored declaration. Not parsed -- it is opaque to
// the device, and only ever compared for equality against what a peer offers.
inline constexpr size_t kDigestLen = 4;

// Smallest legal declaration *body* (shape + button count, no buttons).
inline constexpr size_t kMinBodyLen = 2;

// Body offset of the first button entry: past the mandatory shape byte and the
// button count. Stated once, here, because it has two consumers -- parseBody()
// below and CompanionModeActivity::loadUiDeclaration(), which walks the entries
// itself (see DeclarationInfo's comment on why the entries are not surfaced).
//
// This constant exists because the second consumer was missed when the shape
// byte was introduced: loadUiDeclaration() kept starting its walk at body
// offset 0, read the shape as its button count, and silently produced a garbage
// button map on real hardware while the firmware still built and every host
// test still passed. Two walks that each know the layout independently is the
// bug; one that both read from is the fix.
inline constexpr size_t kBodyFirstButtonOffset = 2;

enum class ParseResult : uint8_t {
  Ok,         // structurally sound and carries a known content shape
  NoShape,    // structurally sound, but the shape byte is absent or unknown
  Malformed,  // truncated, over-long, or an entry that runs off the end
};

// Everything the firmware reads out of a declaration that is not a per-entry
// label. Button and tag *entries* are deliberately not surfaced here: their
// only consumer walks them into its own fixed arrays with its own truncation
// and validity rules (CompanionModeActivity::loadUiDeclaration()), and
// duplicating that walk behind an iterator would buy nothing.
struct DeclarationInfo {
  companionble::ContentShape shape = companionble::ContentShape::Text;
  uint8_t buttonCount = 0;
  uint8_t tagCount = 0;
  // Defaults are the "field absent" answers, so a caller may use these
  // unconditionally after an Ok result without re-checking presence.
  companionble::TagRenderStyle tagRenderStyle = companionble::TagRenderStyle::Bordered;
  uint8_t capabilities = 0;
  // True only when the corresponding optional byte was actually present.
  // Diagnostics and tests want to tell "absent" from "explicitly the default";
  // the render path does not, which is why the resolved values above exist.
  bool hasTagSection = false;
  bool hasTagRenderStyle = false;
  bool hasCapabilities = false;
};

// Parses a declaration *body* -- the bytes after the 4-byte digest, which is
// the form companionpeer::readAssetBody() hands back. `out` may be null when
// the caller only wants the verdict. Nothing is written to `out` unless the
// result is Ok.
ParseResult parseBody(const uint8_t* body, size_t len, DeclarationInfo* out);

// Parses a whole pushed asset, digest included -- the form storeAsset() sees.
// A buffer too short to hold even the digest is Malformed, not NoShape: there
// is no declaration there at all to be missing a shape.
ParseResult parseAsset(const uint8_t* data, size_t len, DeclarationInfo* out);

// True for the three declared values and nothing else -- see ContentShape's
// comment on why unknown is refused rather than reserved.
bool isKnownShape(uint8_t raw);

}  // namespace companionui
