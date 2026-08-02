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

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
