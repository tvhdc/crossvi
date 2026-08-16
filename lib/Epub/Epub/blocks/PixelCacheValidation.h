#pragma once

#include <cstdint>

namespace pixel_cache_validation {

inline bool valid(const uint16_t width, const uint16_t height, const int expectedWidth, const int expectedHeight,
                  const uint64_t fileSize) {
  if (width == 0 || height == 0 || expectedWidth <= 0 || expectedHeight <= 0) return false;
  const int widthDiff = width > expectedWidth ? width - expectedWidth : expectedWidth - width;
  const int heightDiff = height > expectedHeight ? height - expectedHeight : expectedHeight - height;
  if (widthDiff > 1 || heightDiff > 1) return false;

  const uint64_t bytesPerRow = (static_cast<uint64_t>(width) + 3U) / 4U;
  const uint64_t payloadSize = bytesPerRow * height;
  return fileSize == 4U + payloadSize;
}

}  // namespace pixel_cache_validation
