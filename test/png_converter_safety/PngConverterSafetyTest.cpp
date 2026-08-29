#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
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

class ShortWriteSink final : public Print {
 public:
  explicit ShortWriteSink(const size_t failCall) : failCall(failCall) {}
  using Print::write;
  size_t write(const uint8_t*, const size_t length) override {
    ++calls;
    return calls == failCall && length > 0 ? length - 1 : length;
  }

  size_t calls = 0;

 private:
  size_t failCall;
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

uint32_t readLe32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
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

std::vector<uint8_t> makeSingleRowPng(const uint32_t width, const uint8_t colorType, const uint8_t bitDepth,
                                      const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& palette = {},
                                      const std::vector<uint8_t>& transparency = {}) {
  std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  appendBe32(ihdr, width);
  appendBe32(ihdr, 1);
  ihdr.insert(ihdr.end(), {bitDepth, colorType, 0, 0, 0});
  appendChunk(png, "IHDR", ihdr);
  if (!palette.empty()) appendChunk(png, "PLTE", palette);
  if (!transparency.empty()) appendChunk(png, "tRNS", transparency);

  std::vector<uint8_t> scanline = {0};
  scanline.insert(scanline.end(), pixels.begin(), pixels.end());
  appendChunk(png, "IDAT", zlibStored(scanline));
  appendChunk(png, "IEND", {});
  return png;
}

bool convertBgra(const std::vector<uint8_t>& png, ByteSink& output, const int width = 528, const int height = 792) {
  Storage.reset();
  Storage.setFile("/image.png", png);
  HalFile input;
  if (!Storage.openFileForRead("PNG", "/image.png", input)) return false;
  return PngToBmpConverter::pngFileToBgraBmpStreamWithSize(input, output, width, height);
}

TEST(PngConverterSafetyTest, ProductionDecodersAcceptAllFifteenLegalColorDepthPairs) {
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

    HalFile alphaInput;
    ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", alphaInput));
    ByteSink alphaOutput;
    ASSERT_TRUE(PngToBmpConverter::pngFileToBgraBmpStreamWithSize(alphaInput, alphaOutput, 2, 1))
        << "BGRA color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
    ASSERT_GE(alphaOutput.bytes.size(), 2U);
    EXPECT_EQ(alphaOutput.bytes[0], 'B');
    EXPECT_EQ(alphaOutput.bytes[1], 'M');
  }
}

TEST(PngConverterSafetyTest, PackedBmpRejectsPaletteImageWithoutPalette) {
  Storage.reset();
  Storage.setFile("/image.png", makeSingleRowPng(1, 3, 8, {0}));
  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
  ByteSink output;

  EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 1, 1, false));
}

TEST(PngConverterSafetyTest, BgraPreservesRgbaAndDoesNotUpscale) {
  ByteSink output;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(2, 6, 8, {10, 20, 30, 40, 200, 150, 100, 255}), output));

  ASSERT_EQ(output.bytes.size(), 78U);
  EXPECT_EQ(output.bytes[0], 'B');
  EXPECT_EQ(output.bytes[1], 'M');
  EXPECT_EQ(readLe32(output.bytes, 2), 78U);
  EXPECT_EQ(readLe32(output.bytes, 10), 70U);
  EXPECT_EQ(readLe32(output.bytes, 18), 2U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output.bytes, 22)), -1);
  EXPECT_EQ(output.bytes[26], 1);
  EXPECT_EQ(output.bytes[28], 32);
  EXPECT_EQ(readLe32(output.bytes, 30), 3U);
  EXPECT_EQ(readLe32(output.bytes, 54), 0x00FF0000U);
  EXPECT_EQ(readLe32(output.bytes, 58), 0x0000FF00U);
  EXPECT_EQ(readLe32(output.bytes, 62), 0x000000FFU);
  EXPECT_EQ(readLe32(output.bytes, 66), 0xFF000000U);
  const std::vector<uint8_t> expected = {30, 20, 10, 40, 100, 150, 200, 255};
  EXPECT_EQ(std::vector<uint8_t>(output.bytes.begin() + 70, output.bytes.end()), expected);
}

TEST(PngConverterSafetyTest, BgraFitsWithinTargetWithoutChangingAspectRatio) {
  ByteSink output;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(4, 6, 8, {1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24, 31, 32, 33, 34}),
                          output, 2, 2));

  ASSERT_EQ(output.bytes.size(), 78U);
  EXPECT_EQ(readLe32(output.bytes, 18), 2U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output.bytes, 22)), -1);
  const std::vector<uint8_t> expected = {3, 2, 1, 4, 23, 22, 21, 24};
  EXPECT_EQ(std::vector<uint8_t>(output.bytes.begin() + 70, output.bytes.end()), expected);
}

TEST(PngConverterSafetyTest, BgraPreservesGrayAlphaAndIndexedTransparency) {
  ByteSink grayOutput;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(2, 4, 8, {50, 64, 200, 180}), grayOutput));
  const std::vector<uint8_t> expectedGray = {50, 50, 50, 64, 200, 200, 200, 180};
  ASSERT_EQ(std::vector<uint8_t>(grayOutput.bytes.begin() + 70, grayOutput.bytes.end()), expectedGray);

  ByteSink paletteOutput;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(2, 3, 8, {0, 1}, {255, 0, 0, 0, 255, 0}, {0, 128}), paletteOutput));
  const std::vector<uint8_t> expectedPalette = {0, 0, 255, 0, 0, 255, 0, 128};
  EXPECT_EQ(std::vector<uint8_t>(paletteOutput.bytes.begin() + 70, paletteOutput.bytes.end()), expectedPalette);
}

TEST(PngConverterSafetyTest, BgraHonorsGrayscaleAndTruecolorTransparencyKeys) {
  ByteSink grayOutput;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(2, 0, 8, {7, 8}, {}, {0, 7}), grayOutput));
  const std::vector<uint8_t> expectedGray = {7, 7, 7, 0, 8, 8, 8, 255};
  ASSERT_EQ(std::vector<uint8_t>(grayOutput.bytes.begin() + 70, grayOutput.bytes.end()), expectedGray);

  ByteSink rgbOutput;
  ASSERT_TRUE(convertBgra(makeSingleRowPng(2, 2, 8, {1, 2, 3, 4, 5, 6}, {}, {0, 1, 0, 2, 0, 3}), rgbOutput));
  const std::vector<uint8_t> expectedRgb = {3, 2, 1, 0, 6, 5, 4, 255};
  EXPECT_EQ(std::vector<uint8_t>(rgbOutput.bytes.begin() + 70, rgbOutput.bytes.end()), expectedRgb);
}

TEST(PngConverterSafetyTest, BgraRejectsTruncatedCompressedData) {
  std::vector<uint8_t> png = makeSingleRowPng(2, 6, 8, {10, 20, 30, 40, 50, 60, 70, 80});
  constexpr std::array<uint8_t, 4> idatType = {'I', 'D', 'A', 'T'};
  const auto idat = std::search(png.begin(), png.end(), idatType.begin(), idatType.end());
  ASSERT_NE(idat, png.end());
  png.resize(static_cast<size_t>(idat - png.begin()) + 5U);

  ByteSink output;
  EXPECT_FALSE(convertBgra(png, output));

  std::vector<uint8_t> missingEnd = makeSingleRowPng(1, 6, 8, {10, 20, 30, 40});
  ASSERT_GE(missingEnd.size(), 12U);
  missingEnd.resize(missingEnd.size() - 12U);  // Remove IEND length, type, and CRC.
  ByteSink missingEndOutput;
  EXPECT_FALSE(convertBgra(missingEnd, missingEndOutput));
}

TEST(PngConverterSafetyTest, BgraRejectsShortOutputWrites) {
  Storage.reset();
  Storage.setFile("/image.png", makeSingleRowPng(1, 6, 8, {10, 20, 30, 40}));
  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
  ShortWriteSink headerOutput(1);
  EXPECT_FALSE(PngToBmpConverter::pngFileToBgraBmpStreamWithSize(input, headerOutput, 528, 792));
  EXPECT_EQ(headerOutput.calls, 1U);

  HalFile secondInput;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", secondInput));
  ShortWriteSink rowOutput(2);
  EXPECT_FALSE(PngToBmpConverter::pngFileToBgraBmpStreamWithSize(secondInput, rowOutput, 528, 792));
  EXPECT_EQ(rowOutput.calls, 2U);
}

TEST(PngConverterSafetyTest, PackedBmpRejectsShortOutputWrites) {
  Storage.reset();
  Storage.setFile("/image.png", makeSingleRowPng(1, 0, 8, {0}));

  HalFile headerInput;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", headerInput));
  ShortWriteSink headerOutput(1);
  EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(headerInput, headerOutput, 1, 1, false));

  HalFile rowInput;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", rowInput));
  ShortWriteSink rowOutput(63);  // 62-byte 1-bit BMP header, then the packed row.
  EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(rowInput, rowOutput, 1, 1, false));
}

TEST(PngConverterSafetyTest, PackedBmpRejectsMissingPngEnd) {
  std::vector<uint8_t> png = makeSingleRowPng(1, 0, 8, {0});
  ASSERT_GE(png.size(), 12U);
  png.resize(png.size() - 12U);  // Remove IEND length, type, and CRC.

  Storage.reset();
  Storage.setFile("/image.png", png);
  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
  ByteSink output;
  EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 1, 1, false));
}

}  // namespace
