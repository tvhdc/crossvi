#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>

struct PngFramebufferHeader {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bitDepth = 0;
  uint8_t colorType = 0;
};

inline bool parsePngFramebufferHeader(const uint8_t* bytes, const size_t size, PngFramebufferHeader& out) {
  constexpr uint8_t SIGNATURE[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (!bytes || size < 29) return false;
  for (size_t index = 0; index < sizeof(SIGNATURE); ++index) {
    if (bytes[index] != SIGNATURE[index]) return false;
  }
  const auto readBe32 = [bytes](const size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) << 24U | static_cast<uint32_t>(bytes[offset + 1]) << 16U |
           static_cast<uint32_t>(bytes[offset + 2]) << 8U | bytes[offset + 3];
  };
  if (readBe32(8) != 13 || bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R') {
    return false;
  }

  PngFramebufferHeader parsed{readBe32(16), readBe32(20), bytes[24], bytes[25]};
  if (parsed.width == 0 || parsed.height == 0 || parsed.width > INT16_MAX || parsed.height > INT16_MAX ||
      bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0) {
    return false;
  }
  const bool lowOrEight = parsed.bitDepth == 1 || parsed.bitDepth == 2 || parsed.bitDepth == 4 || parsed.bitDepth == 8;
  const bool supported =
      ((parsed.colorType == 0 || parsed.colorType == 3) && lowOrEight) ||
      ((parsed.colorType == 2 || parsed.colorType == 4 || parsed.colorType == 6) && parsed.bitDepth == 8);
  if (!supported) return false;
  out = parsed;
  return true;
}
