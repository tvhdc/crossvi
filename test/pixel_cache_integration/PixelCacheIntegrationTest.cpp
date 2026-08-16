#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"

namespace {

std::vector<uint8_t> cacheBytes(const uint16_t width, const uint16_t height, const size_t payloadBytes) {
  std::vector<uint8_t> bytes(4U + payloadBytes, 0);
  std::memcpy(bytes.data(), &width, sizeof(width));
  std::memcpy(bytes.data() + sizeof(width), &height, sizeof(height));
  return bytes;
}

class FakeDecoder final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig& config) override {
    ++decodeCalls;
    if (!succeed) return false;

    const size_t bytesPerRow = (static_cast<size_t>(config.maxWidth) + 3U) / 4U;
    HalFile cache;
    if (!Storage.openFileForWrite("IMG", config.cachePath, cache)) return false;
    const uint16_t width = static_cast<uint16_t>(config.maxWidth);
    const uint16_t height = static_cast<uint16_t>(config.maxHeight);
    const std::vector<uint8_t> pixels(bytesPerRow * height, 0);
    const bool written = cache.write(&width, sizeof(width)) == sizeof(width) &&
                         cache.write(&height, sizeof(height)) == sizeof(height) &&
                         cache.write(pixels.data(), pixels.size()) == pixels.size();
    cache.close();
    return written;
  }

  const char* getFormatName() const override { return "fake"; }

  bool succeed = true;
  int decodeCalls = 0;
};

class PixelCacheIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    ImageBlock::clearSessionRenderFailures();
    ImageDecoderFactory::decoder = &decoder;
    Storage.setFile("/image.png", {0x01});
  }

  void TearDown() override { ImageDecoderFactory::decoder = nullptr; }

  FakeDecoder decoder;
  GfxRenderer renderer;
};

TEST_F(PixelCacheIntegrationTest, ZeroWidthCacheIsRejectedAndRegenerated) {
  Storage.setFile("/image.pxc", cacheBytes(0, 4, 0));
  ImageBlock block("/image.png", 4, 4);

  EXPECT_TRUE(block.needsDecode());
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_TRUE(block.hasValidCache());
  EXPECT_EQ(renderer.fillRectCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, TruncatedCacheFallsBackToPlaceholderWhenRegenerationFails) {
  Storage.setFile("/image.pxc", cacheBytes(4, 4, 2));  // Four payload bytes are required.
  decoder.succeed = false;
  ImageBlock block("/image.png", 4, 4);

  EXPECT_TRUE(block.needsDecode());
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_FALSE(block.hasValidCache());
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, CacheWithinOnePixelToleranceRendersWithoutDecode) {
  // Expected 4x4; the existing cache contract accepts an independently rounded
  // width and height that differ by one pixel.
  Storage.setFile("/image.pxc", cacheBytes(5, 3, 6));
  ImageBlock block("/image.png", 4, 4);

  EXPECT_FALSE(block.needsDecode());
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, SmallPixelCacheStaysResidentAcrossRepeatedPagePasses) {
  Storage.setFile("/image.pxc", cacheBytes(64, 64, 64U * 64U / 4U));
  ImageBlock block("/image.png", 64, 64);
  Storage.resetIoCounters();

  block.render(renderer, 0, 0);
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(Storage.openReadAttemptsFor("/image.pxc"), 1U);
  EXPECT_EQ(Storage.readCalls(), 3U);
  EXPECT_LE(Storage.maxRead(), 4096U);
}

TEST_F(PixelCacheIntegrationTest, LargePixelCacheKeepsFourKiBStreamingFallback) {
  Storage.setFile("/image.pxc", cacheBytes(400, 400, 400U * 400U / 4U));
  ImageBlock block("/image.png", 400, 400);
  Storage.resetIoCounters();

  block.render(renderer, 0, 0);
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(Storage.openReadAttemptsFor("/image.pxc"), 2U);
  EXPECT_LE(Storage.maxRead(), 4096U);
}

}  // namespace
