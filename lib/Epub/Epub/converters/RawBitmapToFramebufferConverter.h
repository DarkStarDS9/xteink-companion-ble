#pragma once

#include "ImageToFramebufferDecoder.h"

// Decodes the Companion Display Protocol's field 0x04 (image) wire format:
// raw packed 2-bit-per-pixel samples, no header, sized to exactly fill the
// screen. See docs/companion-display-protocol.md's "Image field" section for
// the authoritative wire-format spec.
//
// This replaces PngToFramebufferConverter for field 0x04 only -- PNG decoding
// (PNGdec's ~44KB working set) is a hard wall on this part: measured free
// heap with one BLE peer connected is only ~47-50KB, below PNGdec's own
// ~60KB floor, so an image push could never succeed. This decoder's entire
// working set is one packed row (a few hundred bytes on this panel), so it
// has no such floor. See CLAUDE.md "Memory reality".
//
// PngToFramebufferConverter itself is untouched: it still serves epub/e-book
// image rendering, which is unrelated to this BLE field and out of scope
// here.
class RawBitmapToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  // The interface doesn't pass a renderer here, and this format carries no
  // width/height of its own (see the wire-format note in the .cpp), so the
  // screen dimensions used to sanity-check a staged file's size must be
  // registered ahead of time via setScreenDimensions().
  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override;

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "RAW2BPP"; }

  // Registers the current screen's pixel dimensions, in the same logical
  // (orientation-applied) coordinate space as GfxRenderer::getScreenWidth/
  // Height(). Call once, synchronously, wherever the capability
  // characteristic's px_wide/px_high are computed (CompanionBle.cpp's
  // computeCapabilityValue()) -- those bytes on the wire and this decoder's
  // expected file size must never drift apart.
  static void setScreenDimensions(int widthPx, int heightPx);

  // Packed-row byte count for a given pixel width: 4 pixels/byte, MSB-first
  // (pixel 0 in bits 7-6), row padded out to a whole byte. Matches the
  // on-disk convention PixelCache/DirectCacheWriter already use elsewhere in
  // this codebase (see PixelCache.h), so this is not a new packing scheme,
  // just the wire format adopting an existing one.
  static size_t bytesPerRow(int widthPx) { return (static_cast<size_t>(widthPx) + 3) / 4; }

  // Total expected file size for a full-screen raw image at widthPx x heightPx.
  static size_t expectedFileSize(int widthPx, int heightPx) {
    return bytesPerRow(widthPx) * static_cast<size_t>(heightPx);
  }
};
