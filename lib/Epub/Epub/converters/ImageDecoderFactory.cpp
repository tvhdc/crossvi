#include "ImageDecoderFactory.h"

#include <Logging.h>
#include <Memory.h>

#include <cctype>
#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

namespace {
std::string normalizedExtension(const std::string& imagePath) {
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    std::string ext = imagePath.substr(dotPos);
    for (auto& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
  }
  return {};
}
}  // namespace

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  const std::string ext = normalizedExtension(imagePath);

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      if (!jpegDecoder) LOG_ERR("DEC", "Not enough memory for JPEG decoder");
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      if (!pngDecoder) LOG_ERR("DEC", "Not enough memory for PNG decoder");
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  const std::string ext = normalizedExtension(imagePath);
  return JpegToFramebufferConverter::supportsFormat(ext) || PngToFramebufferConverter::supportsFormat(ext);
}
