#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "ImageDimsProbe.h"
#include "InflateStream.h"
#include "PixelCacheValidation.h"
#include "PngFramebufferPreflight.h"
#include "PngToBmpConverter.h"
#include "PngToBmpConverter/PngImageSafety.h"

namespace allocation_failure_test {
bool enabled = false;
size_t successfulAllocationsBeforeFailure = 0;

bool shouldFail() {
  if (!enabled) return false;
  if (successfulAllocationsBeforeFailure > 0) {
    --successfulAllocationsBeforeFailure;
    return false;
  }
  return true;
}

class ScopedFailure final {
 public:
  explicit ScopedFailure(const size_t successfulAllocations) {
    successfulAllocationsBeforeFailure = successfulAllocations;
    enabled = true;
  }
  ~ScopedFailure() { enabled = false; }
};
}  // namespace allocation_failure_test

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (allocation_failure_test::shouldFail()) return nullptr;
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (allocation_failure_test::shouldFail()) return nullptr;
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { ::operator delete[](ptr); }

class ImageDimensionValidator final : public ImageToFramebufferDecoder {
 public:
  bool validate(const int width, const int height) { return validateImageDimensions(width, height, "test"); }
  bool validateAndStore(const int64_t width, const int64_t height, ImageDimensions& out) {
    return validateAndStoreDimensions(width, height, out, "test");
  }
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig&) override { return false; }
  bool getDimensions(const std::string&, ImageDimensions&) const override { return false; }
  const char* getFormatName() const override { return "test"; }
};

TEST(ImageDimensionValidatorTest, RejectsDimensionsBeforeNarrowingToLayoutStorage) {
  ImageDimensionValidator validator;
  ImageDimensions dimensions{123, 456};

  EXPECT_FALSE(validator.validateAndStore(static_cast<int64_t>(INT16_MAX) + 1, 1, dimensions));
  EXPECT_EQ(dimensions.width, 123);
  EXPECT_EQ(dimensions.height, 456);

  EXPECT_TRUE(validator.validateAndStore(2048, 1536, dimensions));
  EXPECT_EQ(dimensions.width, 2048);
  EXPECT_EQ(dimensions.height, 1536);
}

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

std::vector<uint8_t> pngHeader(const uint32_t width, const uint32_t height) {
  std::vector<uint8_t> bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                                0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52};
  for (const uint32_t value : {width, height}) {
    bytes.push_back(static_cast<uint8_t>(value >> 24U));
    bytes.push_back(static_cast<uint8_t>(value >> 16U));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value));
  }
  return bytes;
}

std::vector<uint8_t> pngIhdr(const uint32_t width, const uint32_t height, const uint8_t bitDepth,
                             const uint8_t colorType, const uint32_t length = 13) {
  auto bytes = pngHeader(width, height);
  bytes[8] = static_cast<uint8_t>(length >> 24U);
  bytes[9] = static_cast<uint8_t>(length >> 16U);
  bytes[10] = static_cast<uint8_t>(length >> 8U);
  bytes[11] = static_cast<uint8_t>(length);
  bytes.insert(bytes.end(), {bitDepth, colorType, 0, 0, 0, 0, 0, 0, 0});
  return bytes;
}

std::vector<uint8_t> pngThroughEmptyIdat(const uint32_t width, const uint32_t height) {
  auto bytes = pngIhdr(width, height, 8, 2);
  bytes.insert(bytes.end(), {0, 0, 0, 0, 'I', 'D', 'A', 'T'});
  return bytes;
}

std::vector<uint8_t> jpegHeader(const uint16_t width, const uint16_t height) {
  // SOI, APP0 (length 4 + two payload bytes), then a baseline SOF0 frame.
  return {0xFF,
          0xD8,
          0xFF,
          0xE0,
          0x00,
          0x04,
          0x4A,
          0x46,
          0xFF,
          0xC0,
          0x00,
          0x11,
          0x08,
          static_cast<uint8_t>(height >> 8U),
          static_cast<uint8_t>(height),
          static_cast<uint8_t>(width >> 8U),
          static_cast<uint8_t>(width),
          0x03};
}

TEST(ImageDimsProbeTest, FindsPngDimensionsAndStopsAfterHeader) {
  const auto bytes = pngHeader(800, 1200);
  ImageDimsProbe probe;
  const size_t consumed = probe.write(bytes.data(), bytes.size());

  ImageDimensions dimensions{};
  EXPECT_TRUE(probe.getDimensions(dimensions));
  EXPECT_EQ(dimensions.width, 800);
  EXPECT_EQ(dimensions.height, 1200);
  EXPECT_LT(consumed, bytes.size());
}

TEST(ImageDimsProbeTest, FindsJpegDimensionsAcrossMarkerSegments) {
  const auto bytes = jpegHeader(600, 900);
  ImageDimsProbe probe;
  const size_t consumed = probe.write(bytes.data(), bytes.size());

  ImageDimensions dimensions{};
  EXPECT_TRUE(probe.getDimensions(dimensions));
  EXPECT_EQ(dimensions.width, 600);
  EXPECT_EQ(dimensions.height, 900);
  EXPECT_LT(consumed, bytes.size());
}

TEST(ImageDimsProbeTest, RejectsTruncatedAndNonImageHeaders) {
  const auto valid = pngHeader(800, 1200);
  for (size_t length = 0; length < valid.size(); ++length) {
    ImageDimsProbe probe;
    probe.write(valid.data(), length);
    ImageDimensions dimensions{};
    EXPECT_FALSE(probe.getDimensions(dimensions)) << "length=" << length;
  }

  ImageDimsProbe textProbe;
  const uint8_t text[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
  textProbe.write(text, sizeof(text));
  ImageDimensions dimensions{};
  EXPECT_FALSE(textProbe.getDimensions(dimensions));
}

TEST(ImageDimsProbeTest, RejectsOversizedPngDimensions) {
  const auto bytes = pngHeader(65536, 1200);
  ImageDimsProbe probe;
  probe.write(bytes.data(), bytes.size());
  ImageDimensions dimensions{};
  EXPECT_FALSE(probe.getDimensions(dimensions));
}

TEST(ImageDimsProbeTest, AcceptsOnlyPngColorDepthPairsDefinedByTheSpecification) {
  const std::vector<std::pair<uint8_t, uint8_t>> validPairs = {
      {0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1},
      {3, 2}, {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16},
  };
  for (const auto& [colorType, bitDepth] : validPairs) {
    EXPECT_TRUE(png_image_safety::validColorDepth(colorType, bitDepth))
        << "color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
  }

  for (const auto& [colorType, bitDepth] :
       std::vector<std::pair<uint8_t, uint8_t>>{{0, 0}, {2, 3}, {3, 16}, {4, 4}, {6, 4}, {1, 8}, {5, 8}}) {
    EXPECT_FALSE(png_image_safety::validColorDepth(colorType, bitDepth))
        << "color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
  }
}

TEST(ImageDimsProbeTest, ValidatesCompleteSupportedPngIhdr) {
  EXPECT_TRUE(png_image_safety::validIhdr(13, 600, 900, 8, 2, 0, 0, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(12, 600, 900, 8, 2, 0, 0, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(13, 0, 900, 8, 2, 0, 0, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(13, 600, 900, 3, 2, 0, 0, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(13, 600, 900, 8, 2, 1, 0, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(13, 600, 900, 8, 2, 0, 1, 0));
  EXPECT_FALSE(png_image_safety::validIhdr(13, 600, 900, 8, 2, 0, 0, 1));
}

TEST(ImageDimsProbeTest, FramebufferPreflightAcceptsOnlyFormatsTheRuntimeDecoderSupports) {
  PngFramebufferHeader header;
  for (const auto& [colorType, bitDepth] :
       std::vector<std::pair<uint8_t, uint8_t>>{{0, 1}, {0, 2}, {0, 4}, {0, 8}, {2, 8}, {3, 1},
                                                {3, 2}, {3, 4}, {3, 8}, {4, 8}, {6, 8}}) {
    const auto bytes = pngIhdr(528, 792, bitDepth, colorType);
    ASSERT_TRUE(parsePngFramebufferHeader(bytes.data(), bytes.size(), header))
        << "color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
    EXPECT_EQ(header.width, 528u);
    EXPECT_EQ(header.height, 792u);
  }

  for (const auto& [colorType, bitDepth] :
       std::vector<std::pair<uint8_t, uint8_t>>{{0, 16}, {2, 16}, {3, 16}, {4, 16}, {6, 16}, {2, 4}}) {
    const auto bytes = pngIhdr(528, 792, bitDepth, colorType);
    EXPECT_FALSE(parsePngFramebufferHeader(bytes.data(), bytes.size(), header))
        << "color=" << static_cast<int>(colorType) << " depth=" << static_cast<int>(bitDepth);
  }
}

TEST(ImageDimsProbeTest, FramebufferPreflightRejectsInterlaceTruncationAndOversize) {
  PngFramebufferHeader header;
  auto interlaced = pngIhdr(528, 792, 8, 6);
  interlaced[28] = 1;
  EXPECT_FALSE(parsePngFramebufferHeader(interlaced.data(), interlaced.size(), header));
  EXPECT_FALSE(parsePngFramebufferHeader(interlaced.data(), 28, header));
  const auto oversized = pngIhdr(static_cast<uint32_t>(INT16_MAX) + 1u, 792, 8, 6);
  EXPECT_FALSE(parsePngFramebufferHeader(oversized.data(), oversized.size(), header));
}

TEST(ImageDimsProbeTest, RejectsPathologicalCropOutputBeforeAllocation) {
  int width = 0;
  int height = 0;
  EXPECT_FALSE(png_image_safety::calculateOutputDimensions(2048, 1, 144, 240, true, width, height));

  EXPECT_TRUE(png_image_safety::calculateOutputDimensions(600, 900, 144, 240, true, width, height));
  EXPECT_EQ(width, 160);
  EXPECT_EQ(height, 240);
  EXPECT_TRUE(png_image_safety::calculateOutputDimensions(600, 900, 144, 240, false, width, height));
  EXPECT_EQ(width, 144);
  EXPECT_EQ(height, 216);
}

TEST(ImageDimsProbeTest, ProductionPngParserRejectsMalformedHeaderWithoutWritingBmp) {
  for (const auto& bytes : {pngIhdr(600, 900, 3, 2), pngIhdr(600, 900, 8, 2, 12), pngIhdr(2048, 1, 8, 2)}) {
    Storage.reset();
    Storage.setFile("/image.png", bytes);
    HalFile input;
    ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
    ByteSink output;
    EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 144, 240, true));
    EXPECT_TRUE(output.bytes.empty());
  }
}

TEST(ImageDimsProbeTest, ProductionPngParserDoesNotWriteHeaderWhenNothrowAllocationFails) {
  const auto bytes = pngThroughEmptyIdat(600, 900);
  for (const size_t successfulNothrowAllocations : {size_t{0}, size_t{1}}) {
    Storage.reset();
    Storage.setFile("/image.png", bytes);
    HalFile input;
    ASSERT_TRUE(Storage.openFileForRead("PNG", "/image.png", input));
    ByteSink output;
    InflateStream::initResult = true;
    {
      allocation_failure_test::ScopedFailure failure(successfulNothrowAllocations);
      EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 144, 240, true));
    }
    InflateStream::initResult = false;
    EXPECT_TRUE(output.bytes.empty());
  }
}

TEST(ImageDimsProbeTest, PixelCacheHeaderRejectsZeroAndTruncatedPayload) {
  EXPECT_FALSE(pixel_cache_validation::valid(0, 1, 1, 1, 4));
  EXPECT_FALSE(pixel_cache_validation::valid(1, 0, 1, 1, 4));
  EXPECT_FALSE(pixel_cache_validation::valid(1, 1, 1, 1, 4));
  EXPECT_TRUE(pixel_cache_validation::valid(1, 1, 1, 1, 5));
  EXPECT_TRUE(pixel_cache_validation::valid(2, 2, 1, 1, 6));
  EXPECT_FALSE(pixel_cache_validation::valid(3, 1, 1, 1, 5));
}

TEST(ImageDimsProbeTest, DimensionValidationCannotOverflowSignedInt) {
  ImageDimensionValidator validator;
  EXPECT_TRUE(validator.validate(2048, 1536));
  EXPECT_FALSE(validator.validate(0, 1536));
  EXPECT_FALSE(validator.validate(-1, 1536));
  EXPECT_FALSE(validator.validate(INT_MAX, INT_MAX));
}
}  // namespace
