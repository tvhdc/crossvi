#pragma once

#include <algorithm>
#include <cstdint>

namespace png_image_safety {

constexpr uint32_t MAX_IMAGE_WIDTH = 2048;
constexpr uint32_t MAX_IMAGE_HEIGHT = 3072;

inline bool validColorDepth(const uint8_t colorType, const uint8_t bitDepth) {
  switch (colorType) {
    case 0:  // Grayscale
      return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16;
    case 2:  // RGB
    case 4:  // Grayscale + alpha
    case 6:  // RGBA
      return bitDepth == 8 || bitDepth == 16;
    case 3:  // Palette
      return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8;
    default:
      return false;
  }
}

inline bool validIhdr(const uint32_t length, const uint32_t width, const uint32_t height, const uint8_t bitDepth,
                      const uint8_t colorType, const uint8_t compression, const uint8_t filter,
                      const uint8_t interlace) {
  return length == 13 && width > 0 && height > 0 && width <= MAX_IMAGE_WIDTH && height <= MAX_IMAGE_HEIGHT &&
         validColorDepth(colorType, bitDepth) && compression == 0 && filter == 0 && interlace == 0;
}

inline bool calculateOutputDimensions(const uint32_t width, const uint32_t height, const int targetWidth,
                                      const int targetHeight, const bool crop, int& outWidth, int& outHeight) {
  if (width == 0 || height == 0 || width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) return false;

  uint64_t scaledWidth = width;
  uint64_t scaledHeight = height;
  if (targetWidth > 0 && targetHeight > 0 &&
      (width != static_cast<uint32_t>(targetWidth) || height != static_cast<uint32_t>(targetHeight))) {
    const uint64_t targetWidth64 = static_cast<uint64_t>(targetWidth);
    const uint64_t targetHeight64 = static_cast<uint64_t>(targetHeight);
    const bool widthScaleIsLarger = targetWidth64 * height > targetHeight64 * width;
    const bool useWidthScale = crop ? widthScaleIsLarger : !widthScaleIsLarger;
    const uint64_t numerator = useWidthScale ? targetWidth64 : targetHeight64;
    const uint64_t denominator = useWidthScale ? width : height;
    scaledWidth = std::max<uint64_t>(1, static_cast<uint64_t>(width) * numerator / denominator);
    scaledHeight = std::max<uint64_t>(1, static_cast<uint64_t>(height) * numerator / denominator);
  }

  if (scaledWidth > MAX_IMAGE_WIDTH || scaledHeight > MAX_IMAGE_HEIGHT) return false;
  outWidth = static_cast<int>(scaledWidth);
  outHeight = static_cast<int>(scaledHeight);
  return true;
}

}  // namespace png_image_safety
