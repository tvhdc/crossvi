#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

struct ImageDimensions {
  int16_t width = 0;
  int16_t height = 0;
};

class PngToFramebufferConverter {
 public:
  static bool getSupportedDimensionsStatic(const std::string& path, ImageDimensions& out) {
    HalFile file;
    if (!Storage.openFileForRead("PNG", path, file)) return false;
    std::array<uint8_t, 29> bytes{};
    const bool read = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = file.close();
    constexpr std::array<uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
    const auto readBe32 = [&bytes](const size_t offset) {
      return static_cast<uint32_t>(bytes[offset]) << 24U | static_cast<uint32_t>(bytes[offset + 1]) << 16U |
             static_cast<uint32_t>(bytes[offset + 2]) << 8U | bytes[offset + 3];
    };
    if (!read || !closed || !std::equal(signature.begin(), signature.end(), bytes.begin()) || readBe32(8) != 13 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0 || bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0) {
      return false;
    }
    const uint32_t width = readBe32(16);
    const uint32_t height = readBe32(20);
    const uint8_t depth = bytes[24];
    const uint8_t color = bytes[25];
    const bool lowOrEight = depth == 1 || depth == 2 || depth == 4 || depth == 8;
    const bool supported =
        ((color == 0 || color == 3) && lowOrEight) || ((color == 2 || color == 4 || color == 6) && depth == 8);
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const uint32_t bytesPerPixel = color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
    const uint64_t pitch = (color == 0 || color == 3) && depth < 8 ? (static_cast<uint64_t>(width) * depth + 7U) / 8U
                                                                   : static_cast<uint64_t>(width) * bytesPerPixel;
    constexpr uint64_t maxBufferedPixels = 16416;
    if (!supported || width == 0 || height == 0 || width > INT16_MAX || height > INT16_MAX || pixels > 3145728U ||
        (pitch + 1U) * 2U + 30U > maxBufferedPixels || width > maxBufferedPixels / 2U) {
      return false;
    }
    out.width = static_cast<int16_t>(width);
    out.height = static_cast<int16_t>(height);
    return true;
  }
};
