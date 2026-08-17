#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCache.h"

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
    if (!writeCache) return true;

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
  bool writeCache = true;
  int decodeCalls = 0;
};

class PixelCacheIntegrationTest : public testing::Test {
 protected:
  static bool extractImage(void* context, const char*, const char* destinationPath) {
    auto& test = *static_cast<PixelCacheIntegrationTest*>(context);
    ++test.extractCalls;
    if (!test.extractSucceeds) return false;
    Storage.setFile(destinationPath, {0x01});
    return true;
  }

  void SetUp() override {
    Storage.reset();
    ImageBlock::clearSessionRenderFailures();
    ImageBlock::setExtractor(this, extractImage);
    ImageDecoderFactory::decoder = &decoder;
    Storage.setFile("/image.png", {0x01});
  }

  void TearDown() override {
    ImageBlock::setExtractor(nullptr, nullptr);
    ImageDecoderFactory::decoder = nullptr;
  }

  FakeDecoder decoder;
  GfxRenderer renderer;
  bool extractSucceeds = true;
  int extractCalls = 0;
};

TEST_F(PixelCacheIntegrationTest, ZeroWidthCacheIsRejectedAndRegenerated) {
  Storage.setFile("/image_4x4.pxc", cacheBytes(0, 4, 0));
  ImageBlock block("/image.png", 4, 4);

  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_TRUE(block.hasValidCache());
  EXPECT_EQ(renderer.fillRectCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, TruncatedCacheFallsBackToPlaceholderWhenRegenerationFails) {
  Storage.setFile("/image_4x4.pxc", cacheBytes(4, 4, 2));  // Four payload bytes are required.
  decoder.succeed = false;
  ImageBlock block("/image.png", 4, 4);

  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_FALSE(block.hasValidCache());
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, CacheWithinOnePixelToleranceRendersWithoutDecode) {
  // Expected 4x4; the existing cache contract accepts an independently rounded
  // width and height that differ by one pixel.
  Storage.setFile("/image_4x4.pxc", cacheBytes(5, 3, 6));
  ImageBlock block("/image.png", 4, 4);

  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, TrailingBytesInvalidatePixelCache) {
  Storage.setFile("/image_4x4.pxc", cacheBytes(4, 4, 5));  // Four payload bytes are required.
  ImageBlock block("/image.png", 4, 4);

  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_TRUE(block.hasValidCache());
}

TEST_F(PixelCacheIntegrationTest, PendingRawPublicationStaysHiddenFromDecoder) {
  Storage.setFile("/image.png.pending", {'P'});
  ImageBlock block("/image.png", "OPS/image.png", 4, 4);

  EXPECT_FALSE(block.imageExists());
  EXPECT_TRUE(block.needsRawPreparation());
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, ReaderRenderDefersMissingRawExtractionWithoutRecordingFailure) {
  ASSERT_TRUE(Storage.remove("/image.png"));
  ImageBlock deferred("/image.png", "OPS/image.png", 4, 4);
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;

  deferred.render(renderer, 0, 0, readBuffer, readBufferCapacity, false);

  EXPECT_TRUE(deferred.awaitsRawPreparation());
  EXPECT_EQ(extractCalls, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 2);

  ImageBlock synchronous("/image.png", "OPS/image.png", 4, 4);
  synchronous.render(renderer, 0, 0);
  EXPECT_EQ(extractCalls, 1);
  EXPECT_EQ(decoder.decodeCalls, 1);
}

TEST_F(PixelCacheIntegrationTest, ReaderRenderDoesNotQueueAnImageWithoutSource) {
  ASSERT_TRUE(Storage.remove("/image.png"));
  ImageBlock block("/image.png", 4, 4);
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;

  block.render(renderer, 0, 0, readBuffer, readBufferCapacity, false);

  EXPECT_FALSE(block.awaitsRawPreparation());
  EXPECT_EQ(extractCalls, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, ReaderRenderQueuesPendingPublicationWithoutOpeningDecoder) {
  Storage.setFile("/image.png.pending", {'P'});
  ImageBlock block("/image.png", "OPS/image.png", 4, 4);
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;

  block.render(renderer, 0, 0, readBuffer, readBufferCapacity, false);

  EXPECT_TRUE(block.awaitsRawPreparation());
  EXPECT_EQ(extractCalls, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, ReaderRenderUsesValidPixelCacheWhenRawImageIsMissing) {
  ASSERT_TRUE(Storage.remove("/image.png"));
  Storage.setFile("/image_4x4.pxc", cacheBytes(4, 4, 4));
  ImageBlock block("/image.png", "OPS/image.png", 4, 4);
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;

  block.render(renderer, 0, 0, readBuffer, readBufferCapacity, false);

  EXPECT_FALSE(block.awaitsRawPreparation());
  EXPECT_EQ(extractCalls, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, PublicationMarkersAreProbedOnceAcrossRepeatedRenderPasses) {
  Storage.setFile("/image_4x4.pxc", cacheBytes(4, 4, 4));
  ImageBlock block("/image.png", 4, 4);
  Storage.resetIoCounters();

  for (int pass = 0; pass < 20; ++pass) block.render(renderer, 0, 0);

  EXPECT_EQ(Storage.existsAttemptsFor("/image.png.pending"), 1U);
  EXPECT_EQ(Storage.existsAttemptsFor("/image.png.bak"), 1U);
  EXPECT_EQ(decoder.decodeCalls, 0);
}

TEST_F(PixelCacheIntegrationTest, PendingPublicationSnapshotStaysHiddenAcrossRepeatedRenderPasses) {
  Storage.setFile("/image.png.pending", {'P'});
  ImageBlock block("/image.png", "OPS/image.png", 4, 4);
  Storage.resetIoCounters();

  for (int pass = 0; pass < 20; ++pass) block.render(renderer, 0, 0);

  EXPECT_EQ(Storage.existsAttemptsFor("/image.png.pending"), 1U);
  EXPECT_EQ(Storage.existsAttemptsFor("/image.png.bak"), 0U);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 40);
}

TEST_F(PixelCacheIntegrationTest, PreparationFailureRemainsSuppressedUntilExplicitReset) {
  ImageBlock::markPreparationFailure("/image.png");
  ImageBlock failed("/image.png", 4, 4);
  failed.render(renderer, 0, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock retried("/image.png", 4, 4);
  retried.render(renderer, 0, 0);
  EXPECT_EQ(decoder.decodeCalls, 1);
}

TEST_F(PixelCacheIntegrationTest, FailedPendingPublicationDoesNotQueueAgainUntilReset) {
  Storage.setFile("/image.png.pending", {'P'});
  ImageBlock::markPreparationFailure("/image.png");
  ImageBlock block("/image.png", "OPS/image.png", 4, 4);
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;

  EXPECT_FALSE(block.needsRawPreparation());
  block.render(renderer, 0, 0, readBuffer, readBufferCapacity, false);

  EXPECT_FALSE(block.awaitsRawPreparation());
  EXPECT_EQ(extractCalls, 0);
  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, DifferentLayoutSizesKeepIndependentPixelCaches) {
  ImageBlock small("/image.png", 4, 4);
  ImageBlock large("/image.png", 8, 8);

  small.render(renderer, 0, 0);
  large.render(renderer, 0, 0);

  EXPECT_TRUE(Storage.exists("/image_4x4.pxc"));
  EXPECT_TRUE(Storage.exists("/image_8x8.pxc"));
  EXPECT_EQ(decoder.decodeCalls, 2);
  EXPECT_TRUE(small.hasValidCache());
  EXPECT_TRUE(large.hasValidCache());
}

TEST_F(PixelCacheIntegrationTest, WriterPublishesOnlyAfterFinalize) {
  PixelCache cache;

  ASSERT_TRUE(cache.begin("/image.pxc", 4, 4, 0, 0, 1));
  EXPECT_FALSE(Storage.exists("/image.pxc"));
  EXPECT_TRUE(Storage.exists("/image.pxc.tmp"));

  ASSERT_TRUE(cache.finalize());
  EXPECT_TRUE(Storage.exists("/image.pxc"));
  EXPECT_FALSE(Storage.exists("/image.pxc.tmp"));
  EXPECT_EQ(Storage.file("/image.pxc").size(), 8U);
}

TEST_F(PixelCacheIntegrationTest, SyncFailurePreservesPreviousCache) {
  const std::vector<uint8_t> previous = cacheBytes(4, 4, 4);
  Storage.setFile("/image.pxc", previous);
  PixelCache cache;

  ASSERT_TRUE(cache.begin("/image.pxc", 4, 4, 0, 0, 1));
  Storage.failSyncOnce();

  EXPECT_FALSE(cache.finalize());
  EXPECT_EQ(Storage.file("/image.pxc"), previous);
  EXPECT_FALSE(Storage.exists("/image.pxc.tmp"));
}

TEST_F(PixelCacheIntegrationTest, PublishFailureLeavesARegenerableCacheMiss) {
  Storage.setFile("/image.pxc", cacheBytes(0, 4, 0));
  PixelCache cache;

  ASSERT_TRUE(cache.begin("/image.pxc", 4, 4, 0, 0, 1));
  Storage.failRenameTo("/image.pxc");

  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists("/image.pxc"));
  EXPECT_FALSE(Storage.exists("/image.pxc.tmp"));
  EXPECT_FALSE(Storage.exists("/image.pxc.bak"));
}

TEST_F(PixelCacheIntegrationTest, SuccessfulDecodeWithoutCacheIsNotRepeatedForSamePageBlock) {
  decoder.writeCache = false;
  ImageBlock block("/image.png", 4, 4);

  block.render(renderer, 0, 0);
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 1);
  EXPECT_EQ(renderer.fillRectCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, MoreThanSixteenFailedImagesAreNotDecodedAgainWithinSamePageBlocks) {
  decoder.succeed = false;
  std::vector<std::unique_ptr<ImageBlock>> blocks;
  for (int i = 0; i < 17; ++i) {
    const std::string path = "/broken_" + std::to_string(i) + ".png";
    Storage.setFile(path, {0x01});
    blocks.emplace_back(new ImageBlock(path, 4, 4));
    blocks.back()->render(renderer, 0, 0);
  }

  blocks.back()->render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 17);
}

TEST_F(PixelCacheIntegrationTest, MoreThanSixteenPreparationFailuresRemainSuppressedAfterDeserialization) {
  std::vector<std::string> failedPaths;
  for (int i = 0; i < 32; ++i) {
    failedPaths.emplace_back("/async_broken_" + std::to_string(i) + ".png");
    const std::string& path = failedPaths.back();
    Storage.setFile(path, {0x01});
    ImageBlock::markPreparationFailure(path);
  }

  for (const std::string& path : failedPaths) {
    ImageBlock reloaded(path, 4, 4);
    reloaded.render(renderer, 0, 0);
  }
  EXPECT_EQ(decoder.decodeCalls, 0);

  Storage.setFile("/healthy.png", {0x01});
  ImageBlock unrelated("/healthy.png", 4, 4);
  unrelated.render(renderer, 0, 0);
  EXPECT_EQ(decoder.decodeCalls, 1);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock retried(failedPaths.front(), 4, 4);
  retried.render(renderer, 0, 0);
  EXPECT_EQ(decoder.decodeCalls, 2);
}

TEST_F(PixelCacheIntegrationTest, SmallPixelCacheStaysResidentAcrossRepeatedPagePasses) {
  Storage.setFile("/image_64x64.pxc", cacheBytes(64, 64, 64U * 64U / 4U));
  ImageBlock block("/image.png", 64, 64);
  Storage.resetIoCounters();

  block.render(renderer, 0, 0);
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(Storage.openReadAttemptsFor("/image_64x64.pxc"), 1U);
  EXPECT_EQ(Storage.readCalls(), 3U);
  EXPECT_LE(Storage.maxRead(), 4096U);
}

TEST_F(PixelCacheIntegrationTest, LargePixelCacheKeepsFourKiBStreamingFallback) {
  Storage.setFile("/image_400x400.pxc", cacheBytes(400, 400, 400U * 400U / 4U));
  ImageBlock block("/image.png", 400, 400);
  Storage.resetIoCounters();

  block.render(renderer, 0, 0);
  block.render(renderer, 0, 0);

  EXPECT_EQ(decoder.decodeCalls, 0);
  EXPECT_EQ(Storage.openReadAttemptsFor("/image_400x400.pxc"), 2U);
  EXPECT_LE(Storage.maxRead(), 4096U);
}

}  // namespace
