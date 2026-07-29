#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;
class RawBitmapToFramebufferConverter;

class ImageDecoderFactory {
 public:
  // Returns non-owning pointer - factory owns the decoder lifetime
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
  // Serves the BLE companion protocol's field 0x04 (.raw staged files) only —
  // unrelated to the epub/e-book PNG/JPEG image path above.
  static std::unique_ptr<RawBitmapToFramebufferConverter> rawDecoder;
};
