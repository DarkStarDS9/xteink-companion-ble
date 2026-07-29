#include "RawBitmapToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "DirectPixelWriter.h"

namespace {

// Registered once via RawBitmapToFramebufferConverter::setScreenDimensions().
// This firmware has exactly one physical panel and one live decoder instance,
// so a file-scope pair is simpler than plumbing a renderer reference through
// the ImageToFramebufferDecoder interface (getDimensions() has none).
int g_screenWidthPx = 0;
int g_screenHeightPx = 0;

}  // namespace

void RawBitmapToFramebufferConverter::setScreenDimensions(int widthPx, int heightPx) {
  g_screenWidthPx = widthPx;
  g_screenHeightPx = heightPx;
}

bool RawBitmapToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::checkFileExtension(extension, ".raw");
}

bool RawBitmapToFramebufferConverter::getDimensions(const std::string& imagePath, ImageDimensions& dims) const {
  if (g_screenWidthPx <= 0 || g_screenHeightPx <= 0) {
    LOG_ERR("RAW2BPP", "screen dimensions not registered");
    return false;
  }

  HalFile f;
  if (!Storage.openFileForRead("RAW2BPP", imagePath, f)) {
    LOG_ERR("RAW2BPP", "cannot open %s", imagePath.c_str());
    return false;
  }
  const size_t size = f.size();
  f.close();

  const size_t expected = expectedFileSize(g_screenWidthPx, g_screenHeightPx);
  if (size != expected) {
    LOG_ERR("RAW2BPP", "%s is %u bytes, expected exactly %u for %dx%d packed 2bpp", imagePath.c_str(),
            static_cast<unsigned>(size), static_cast<unsigned>(expected), g_screenWidthPx, g_screenHeightPx);
    return false;
  }

  dims.width = static_cast<int16_t>(g_screenWidthPx);
  dims.height = static_cast<int16_t>(g_screenHeightPx);
  return true;
}

bool RawBitmapToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                          const RenderConfig& /*config*/) {
  // The wire format carries no scale/crop/offset of its own -- it is always
  // exactly the current screen, pixel for pixel (see the class comment and
  // the protocol doc). RenderConfig's x/y/maxWidth/maxHeight/useExactDimensions
  // exist for the PNG/JPEG epub-image path and don't apply here; companion
  // mode's renderImage() doesn't set them for this reason.
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const size_t rowBytes = bytesPerRow(width);
  const size_t expected = expectedFileSize(width, height);

  HalFile f;
  if (!Storage.openFileForRead("RAW2BPP", imagePath, f)) {
    LOG_ERR("RAW2BPP", "cannot open %s", imagePath.c_str());
    return false;
  }
  const ScopedCleanup cleanup{[&f]() { f.close(); }};

  if (f.size() != expected) {
    LOG_ERR("RAW2BPP", "%s is %u bytes, expected exactly %u for %dx%d packed 2bpp", imagePath.c_str(),
            static_cast<unsigned>(f.size()), static_cast<unsigned>(expected), width, height);
    return false;
  }

  // Single packed-row scratch buffer: the entire working set of this decoder.
  // For this panel that's on the order of a few hundred bytes, versus
  // PNGdec's ~44KB inflate window -- see the class comment for why that
  // matters.
  auto rowBuf = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!rowBuf) {
    LOG_ERR("RAW2BPP", "OOM allocating %u-byte row buffer", static_cast<unsigned>(rowBytes));
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int y = 0; y < height; y++) {
    if (static_cast<size_t>(f.read(rowBuf.get(), rowBytes)) != rowBytes) {
      LOG_ERR("RAW2BPP", "short read at row %d of %s", y, imagePath.c_str());
      return false;
    }

    pw.beginRow(y);
    for (int x = 0; x < width; x++) {
      // MSB-first, 4 pixels/byte: pixel 0 lives in bits 7-6 of byte 0. Matches
      // PixelCache/DirectCacheWriter's convention (see PixelCache.h) rather
      // than inventing a new one.
      const uint8_t byteVal = rowBuf[x >> 2];
      const int shift = 6 - ((x & 3) * 2);
      const uint8_t sample = (byteVal >> shift) & 0x03;
      pw.writePixel(x, sample);
    }
  }

  return true;
}
