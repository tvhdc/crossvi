#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  const uint64_t pixels = width > 0 && height > 0 ? static_cast<uint64_t>(width) * static_cast<uint64_t>(height) : 0;
  if (pixels == 0 || pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Invalid image dimensions (%dx%d, %llu pixels %s), max supported: %d pixels", width, height,
            static_cast<unsigned long long>(pixels), format.c_str(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

bool ImageToFramebufferDecoder::validateAndStoreDimensions(const int64_t width, const int64_t height,
                                                           ImageDimensions& out, const char* format) {
  if (width <= 0 || height <= 0 || width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION) {
    LOG_ERR("IMG", "Invalid %s dimensions: %lldx%lld (max %lld per dimension)", format,
            static_cast<long long>(width), static_cast<long long>(height),
            static_cast<long long>(MAX_SOURCE_DIMENSION));
    return false;
  }

  if (!validateImageDimensions(static_cast<int>(width), static_cast<int>(height), format)) return false;

  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
