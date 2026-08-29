#include <Arduino.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "SleepImageNormalizer.h"
#include "SleepImageValidation.h"

namespace {

using SleepImageNormalizer::Result;
using SleepImageNormalizer::Status;
using SleepImageSelectionStore::Target;

void appendLe16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

void appendBe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value));
}

uint16_t readLe16(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint16_t>(bytes.at(offset)) | static_cast<uint16_t>(bytes.at(offset + 1)) << 8U;
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes.at(offset)) | static_cast<uint32_t>(bytes.at(offset + 1)) << 8U |
         static_cast<uint32_t>(bytes.at(offset + 2)) << 16U | static_cast<uint32_t>(bytes.at(offset + 3)) << 24U;
}

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  uint32_t first = 1;
  uint32_t second = 0;
  for (const uint8_t byte : bytes) {
    first = (first + byte) % 65521U;
    second = (second + first) % 65521U;
  }
  return second << 16U | first;
}

std::vector<uint8_t> storedZlib(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> compressed = {0x78, 0x01, 0x01};
  const uint16_t shortLength = static_cast<uint16_t>(raw.size());
  const uint16_t inverse = static_cast<uint16_t>(~shortLength);
  appendLe16(compressed, shortLength);
  appendLe16(compressed, inverse);
  compressed.insert(compressed.end(), raw.begin(), raw.end());
  appendBe32(compressed, adler32(raw));
  return compressed;
}

void appendChunk(std::vector<uint8_t>& png, const char (&type)[5], const std::vector<uint8_t>& payload) {
  appendBe32(png, static_cast<uint32_t>(payload.size()));
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), payload.begin(), payload.end());
  appendBe32(png, 0);  // Production's streaming converter does not consume chunk CRCs.
}

std::vector<uint8_t> rgbaPng(const uint32_t width = 2, const uint32_t height = 1, const uint8_t filter = 0) {
  std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  appendBe32(ihdr, width);
  appendBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  appendChunk(png, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1U + static_cast<size_t>(width) * 4U));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(filter);
    for (uint32_t x = 0; x < width; ++x) {
      raw.push_back(static_cast<uint8_t>(10U + x));
      raw.push_back(static_cast<uint8_t>(20U + y));
      raw.push_back(30);
      raw.push_back(x == 0 ? 0 : x == 1 ? 128 : 255);
    }
  }
  appendChunk(png, "IDAT", storedZlib(raw));
  appendChunk(png, "IEND", {});
  return png;
}

std::vector<uint8_t> rgba16Png() {
  std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  appendBe32(ihdr, 1);
  appendBe32(ihdr, 1);
  ihdr.insert(ihdr.end(), {16, 6, 0, 0, 0});
  appendChunk(png, "IHDR", ihdr);
  appendChunk(png, "IDAT", storedZlib({0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0x80, 0x00}));
  appendChunk(png, "IEND", {});
  return png;
}

std::vector<uint8_t> bmp24(const int width, const int height) {
  const uint32_t rowBytes = (static_cast<uint32_t>(width) * 3U + 3U) / 4U * 4U;
  const uint32_t imageBytes = rowBytes * static_cast<uint32_t>(height);
  const uint32_t fileBytes = 54U + imageBytes;
  std::vector<uint8_t> bmp;
  bmp.reserve(fileBytes);
  appendLe16(bmp, 0x4D42);
  appendLe32(bmp, fileBytes);
  appendLe32(bmp, 0);
  appendLe32(bmp, 54);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(width));
  appendLe32(bmp, static_cast<uint32_t>(-height));
  appendLe16(bmp, 1);
  appendLe16(bmp, 24);
  appendLe32(bmp, 0);
  appendLe32(bmp, imageBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);
  bmp.resize(fileBytes);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = bmp.data() + 54U + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      const uint8_t gray = static_cast<uint8_t>((x + y) & 0xFF);
      row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = gray;
    }
  }
  return bmp;
}

std::vector<uint8_t> bmp32BitfieldsWithoutAlphaMask(const int width, const int height) {
  const uint32_t rowBytes = static_cast<uint32_t>(width) * 4U;
  const uint32_t imageBytes = rowBytes * static_cast<uint32_t>(height);
  const uint32_t fileBytes = 70U + imageBytes;
  std::vector<uint8_t> bmp;
  bmp.reserve(fileBytes);
  appendLe16(bmp, 0x4D42);
  appendLe32(bmp, fileBytes);
  appendLe32(bmp, 0);
  appendLe32(bmp, 70);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(width));
  appendLe32(bmp, static_cast<uint32_t>(-height));
  appendLe16(bmp, 1);
  appendLe16(bmp, 32);
  appendLe32(bmp, 3);
  appendLe32(bmp, imageBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0x00FF0000U);
  appendLe32(bmp, 0x0000FF00U);
  appendLe32(bmp, 0x000000FFU);
  appendLe32(bmp, 0);  // Valid ordinary BMP, but not the strict BGRA overlay contract.
  bmp.resize(fileBytes);
  for (uint32_t offset = 70; offset < fileBytes; offset += 4) {
    bmp[offset] = 30;
    bmp[offset + 1] = 20;
    bmp[offset + 2] = 10;
    bmp[offset + 3] = 255;
  }
  return bmp;
}

std::vector<uint8_t> bmp32RgbWithReservedZero(const int width, const int height) {
  const uint32_t rowBytes = static_cast<uint32_t>(width) * 4U;
  const uint32_t imageBytes = rowBytes * static_cast<uint32_t>(height);
  const uint32_t fileBytes = 54U + imageBytes;
  std::vector<uint8_t> bmp;
  bmp.reserve(fileBytes);
  appendLe16(bmp, 0x4D42);
  appendLe32(bmp, fileBytes);
  appendLe32(bmp, 0);
  appendLe32(bmp, 54);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(width));
  appendLe32(bmp, static_cast<uint32_t>(-height));
  appendLe16(bmp, 1);
  appendLe16(bmp, 32);
  appendLe32(bmp, 0);
  appendLe32(bmp, imageBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);
  bmp.resize(fileBytes);
  for (uint32_t offset = 54; offset < fileBytes; offset += 4) {
    bmp[offset] = 30;
    bmp[offset + 1] = 20;
    bmp[offset + 2] = 10;
    bmp[offset + 3] = 0;  // Reserved in BI_RGB, not an alpha channel.
  }
  return bmp;
}

class SleepImageNormalizerTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    resetTestYieldCount();
  }
};

TEST_F(SleepImageNormalizerTest, RejectsSourceAboveCapBeforeReadingOrWriting) {
  const auto source = rgbaPng();
  Storage.setFile("/too-large.png", source);
  Storage.reportFileSize("/too-large.png", SleepImageNormalizer::MAX_SOURCE_BYTES + 1U);

  const Result result = SleepImageNormalizer::prepare("/too-large.png", true, 528, 792);

  EXPECT_EQ(result.status, Status::TooLarge);
  EXPECT_EQ(result.sourceBytes, SleepImageNormalizer::MAX_SOURCE_BYTES + 1U);
  EXPECT_EQ(Storage.readCalls(), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/sleep-overlay.png.tmp"), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/sleep-overlay.bmp.tmp"), 0U);
  EXPECT_EQ(Storage.file("/too-large.png"), source);
}

TEST_F(SleepImageNormalizerTest, NormalizesLargeBmpToBoundedTwoBitX3Bitmap) {
  const auto source = bmp24(800, 400);
  Storage.setFile("/large.bmp", source);

  const Result result = SleepImageNormalizer::prepare("/large.bmp", false, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::NormalBmp);
  EXPECT_TRUE(result.optimized);
  ASSERT_TRUE(Storage.exists("/sleep.bmp.tmp"));
  const auto& output = Storage.file("/sleep.bmp.tmp");
  ASSERT_GE(output.size(), 70U);
  EXPECT_EQ(readLe32(output, 18), 528U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output, 22)), -264);
  EXPECT_EQ(readLe16(output, 28), 2U);
  EXPECT_EQ(readLe32(output, 2), 34918U);
  EXPECT_EQ(result.outputBytes, output.size());
  EXPECT_LT(output.size(), source.size());
  EXPECT_EQ(Storage.file("/large.bmp"), source);
  EXPECT_GT(TestYieldCount, 0U);
}

TEST_F(SleepImageNormalizerTest, NormalizingSmallHighColorBmpDoesNotUpscaleIt) {
  const auto source = bmp24(2, 1);
  Storage.setFile("/small.bmp", source);

  const Result result = SleepImageNormalizer::prepare("/small.bmp", false, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  ASSERT_TRUE(result.optimized);
  const auto& output = Storage.file("/sleep.bmp.tmp");
  EXPECT_EQ(readLe32(output, 18), 2U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output, 22)), -1);
  EXPECT_EQ(readLe16(output, 28), 2U);
  EXPECT_EQ(output.size(), 74U);
  EXPECT_EQ(Storage.file("/small.bmp"), source);
}

TEST_F(SleepImageNormalizerTest, NormalBmpAcceptsBitmapCompatibleBitfieldsWithoutOverlayAlphaMask) {
  const auto source = bmp32BitfieldsWithoutAlphaMask(2, 1);
  Storage.setFile("/ordinary-bitfields.bmp", source);

  const Result result = SleepImageNormalizer::prepare("/ordinary-bitfields.bmp", false, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::NormalBmp);
  EXPECT_TRUE(result.optimized);
  ASSERT_TRUE(Storage.exists("/sleep.bmp.tmp"));
  const auto& output = Storage.file("/sleep.bmp.tmp");
  EXPECT_EQ(readLe32(output, 18), 2U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output, 22)), -1);
  EXPECT_EQ(readLe16(output, 28), 2U);
  EXPECT_EQ(Storage.file("/ordinary-bitfields.bmp"), source);
}

TEST_F(SleepImageNormalizerTest, TransparentBmpKeepsOrdinaryBitmapAsWhiteKeyOverlay) {
  const auto source = bmp24(2, 1);
  Storage.setFile("/white-key.bmp", source);

  const Result result = SleepImageNormalizer::prepare("/white-key.bmp", true, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayBmp);
  EXPECT_FALSE(result.optimized);
  EXPECT_EQ(Storage.file("/sleep-overlay.bmp.tmp"), source);
}

TEST_F(SleepImageNormalizerTest, TransparentBmpAcceptsOrdinaryThirtyTwoBitBitfieldsAsWhiteKey) {
  const auto source = bmp32BitfieldsWithoutAlphaMask(2, 1);
  Storage.setFile("/ordinary-bitfields.bmp", source);

  const Result result = SleepImageNormalizer::prepare("/ordinary-bitfields.bmp", true, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayBmp);
  EXPECT_FALSE(result.optimized);
  EXPECT_EQ(Storage.file("/sleep-overlay.bmp.tmp"), source);
  EXPECT_EQ(Storage.file("/ordinary-bitfields.bmp"), source);
}

TEST_F(SleepImageNormalizerTest, TransparentBmpTreatsBiRgbReservedByteAsOpaqueImageData) {
  const auto source = bmp32RgbWithReservedZero(2, 1);
  Storage.setFile("/ordinary-rgb32.bmp", source);

  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("SLP", "/ordinary-rgb32.bmp", input));
  SleepImageValidation::Bmp32Header header;
  EXPECT_EQ(SleepImageValidation::readBmp32Header(input, header), SleepImageValidation::Bmp32HeaderStatus::Not32Bit);
  ASSERT_TRUE(input.close());

  const Result result = SleepImageNormalizer::prepare("/ordinary-rgb32.bmp", true, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayBmp);
  EXPECT_FALSE(result.optimized);
  EXPECT_EQ(Storage.file("/sleep-overlay.bmp.tmp"), source);
  EXPECT_EQ(Storage.file("/ordinary-rgb32.bmp"), source);
}

TEST_F(SleepImageNormalizerTest, FullyValidatesAndKeepsCompactOverlayPng) {
  const auto source = rgbaPng();
  Storage.setFile("/overlay.png", source);

  const Result result = SleepImageNormalizer::prepare("/overlay.png", true, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayPng);
  EXPECT_FALSE(result.optimized);
  EXPECT_EQ(result.sourceBytes, source.size());
  EXPECT_EQ(result.outputBytes, source.size());
  EXPECT_EQ(Storage.file("/sleep-overlay.png.tmp"), source);
  EXPECT_EQ(Storage.file("/overlay.png"), source);
}

TEST_F(SleepImageNormalizerTest, ConvertsCompactRgba16PngWhenOverlayRendererCannotUseItDirectly) {
  const auto source = rgba16Png();
  Storage.setFile("/rgba16.png", source);

  const Result result = SleepImageNormalizer::prepare("/rgba16.png", true, 528, 792);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayBmp);
  EXPECT_TRUE(result.optimized);
  ASSERT_TRUE(Storage.exists("/sleep-overlay.bmp.tmp"));
  const auto& output = Storage.file("/sleep-overlay.bmp.tmp");
  ASSERT_EQ(output.size(), 74U);
  EXPECT_EQ(readLe32(output, 18), 1U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output, 22)), -1);
  EXPECT_EQ(readLe16(output, 28), 32U);
  EXPECT_EQ(output[73], 0x80U);
  EXPECT_EQ(Storage.file("/rgba16.png"), source);
}

TEST_F(SleepImageNormalizerTest, RejectsHeaderValidPngWithInvalidScanlineAndCleansTemps) {
  const auto previous = rgbaPng();
  const auto broken = rgbaPng(2, 1, 5);
  Storage.setFile("/sleep-overlay.png", previous);
  Storage.setFile("/sleep-overlay.bmp.tmp", {'B', 'A', 'D'});
  Storage.setFile("/broken.png", broken);

  const Result result = SleepImageNormalizer::prepare("/broken.png", true, 528, 792);

  EXPECT_EQ(result.status, Status::Invalid);
  EXPECT_EQ(Storage.file("/sleep-overlay.png"), previous);
  EXPECT_EQ(Storage.file("/broken.png"), broken);
  EXPECT_FALSE(Storage.exists("/sleep.bmp.tmp"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.bmp.tmp"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.png.tmp"));
}

TEST_F(SleepImageNormalizerTest, FitsOversizedOverlayPngAndPreservesAlphaInBgraBitmap) {
  const auto source = rgbaPng(4, 1);
  Storage.setFile("/wide-overlay.png", source);

  const Result result = SleepImageNormalizer::prepare("/wide-overlay.png", true, 2, 2);

  ASSERT_EQ(result.status, Status::Ready);
  EXPECT_EQ(result.target, Target::OverlayBmp);
  EXPECT_TRUE(result.optimized);
  ASSERT_TRUE(Storage.exists("/sleep-overlay.bmp.tmp"));
  const auto& output = Storage.file("/sleep-overlay.bmp.tmp");
  ASSERT_EQ(output.size(), 78U);
  EXPECT_EQ(readLe32(output, 18), 2U);
  EXPECT_EQ(static_cast<int32_t>(readLe32(output, 22)), -1);
  EXPECT_EQ(readLe16(output, 28), 32U);
  EXPECT_EQ(output[73], 0U);
  EXPECT_EQ(output[77], 255U);
  EXPECT_EQ(result.outputBytes, output.size());
  EXPECT_EQ(Storage.file("/wide-overlay.png"), source);
}

TEST_F(SleepImageNormalizerTest, OutputFailureLeavesSourceUntouchedAndRemovesTemp) {
  const auto source = rgbaPng();
  Storage.setFile("/normal.png", source);
  Storage.shortWriteFor("/sleep.bmp.tmp");

  const Result result = SleepImageNormalizer::prepare("/normal.png", false, 528, 792);

  EXPECT_EQ(result.status, Status::IoError);
  EXPECT_EQ(Storage.file("/normal.png"), source);
  EXPECT_FALSE(Storage.exists("/sleep.bmp.tmp"));
}

}  // namespace
