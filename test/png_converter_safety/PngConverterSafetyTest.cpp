#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "PngToBmpConverter.h"

namespace {

class ByteSink final : public Print {
 public:
  using Print::write;
  size_t write(const uint8_t* data, const size_t length) override {
    bytes.insert(bytes.end(), data, data + length);
    return length;
  }

  std::vector<uint8_t> bytes;
};

void appendBe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value));
}

uint32_t adler32(const std::vector<uint8_t>& data) {
  uint32_t first = 1;
  uint32_t second = 0;
  for (const uint8_t byte : data) {
    first = (first + byte) % 65521U;
    second = (second + first) % 65521U;
  }
  return (second << 16U) | first;
}

std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> compressed = {0x78, 0x01, 0x01};
  const uint16_t length = static_cast<uint16_t>(raw.size());
  const uint16_t inverseLength = static_cast<uint16_t>(~length);
  compressed.push_back(static_cast<uint8_t>(length));
  compressed.push_back(static_cast<uint8_t>(length >> 8U));
  compressed.push_back(static_cast<uint8_t>(inverseLength));
  compressed.push_back(static_cast<uint8_t>(inverseLength >> 8U));
  compressed.insert(compressed.end(), raw.begin(), raw.end());
  appendBe32(compressed, adler32(raw));
  return compressed;
}

void appendChunk(std::vector<uint8_t>& png, const char (&type)[5], const std::vector<uint8_t>& payload) {
  appendBe32(png, static_cast<uint32_t>(payload.size()));
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), payload.begin(), payload.end());
  appendBe32(png, 0);  // The production decoder streams pixels and does not consume chunk CRCs.
}

size_t rawRowBytes(const uint8_t colorType, const uint8_t bitDepth, const uint32_t width) {
  uint8_t channels = 1;
  if (colorType == 2) channels = 3;
  if (colorType == 4) channels = 2;
  if (colorType == 6) channels = 4;
  const uint32_t bits = width * channels * bitDepth;
  return (bits + 7U) / 8U;
}

std::vector<uint8_t> makePng(const uint8_t colorType, const uint8_t bitDepth) {
  constexpr uint32_t width = 2;
  constexpr uint32_t height = 1;
  std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  appendBe32(ihdr, width);
  appendBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {bitDepth, colorType, 0, 0, 0});
  appendChunk(png, "IHDR", ihdr);

  if (colorType == 3) {
    const size_t entries = size_t{1} << bitDepth;
    std::vector<uint8_t> palette(entries * 3U);
    for (size_t entry = 0; entry < entries; ++entry) {
      const uint8_t gray = static_cast<uint8_t>(entry * 255U / (entries - 1U));
      palette[entry * 3U] = gray;
      palette[entry * 3U + 1U] = gray;
      palette[entry * 3U + 2U] = gray;
    }
    appendChunk(png, "PLTE", palette);
  }

  std::vector<uint8_t> scanline(1U + rawRowBytes(colorType, bitDepth, width), 0);
  appendChunk(png, "IDAT", zlibStored(scanline));
  appendChunk(png, "IEND", {});
  return png;
}

TEST(PngConverterSafetyTest, ProductionDecoderAcceptsAllFifteenLegalColorDepthPairs) {
  constexpr std::array<std::pair<uint8_t, uint8_t>, 15> validPairs = {
      std::pair<uint8_t, uint8_t>{0, 1},
      {0, 2},
      {0, 4},
      {0, 8},
      {0, 16},
      {2, 8},
      {2, 16},
      {3, 1},
      {3, 2},
      {3, 4},
      {3, 8},
      {4, 8},
      {4, 16},
      {6, 8},
      {6, 16},
  };

  for (const auto& [colorType, bitDepth] : validPairs) {
    Storage.reset();
    Storage.setFile("/image.png", makePng(colorType, bitDepth));
    HalFile input;
    ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
    ByteSink output;
    ASSERT_TRUE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 2, 1, false))
        << "color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
    ASSERT_GE(output.bytes.size(), 2U);
    EXPECT_EQ(output.bytes[0], 'B');
    EXPECT_EQ(output.bytes[1], 'M');
  }
}

}  // namespace
