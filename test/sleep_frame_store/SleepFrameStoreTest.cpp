#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "SleepFrameStore.h"

namespace {
constexpr char FRAME[] = "/.crosspoint/sleep_frame.bin";
constexpr char BACKUP[] = "/.crosspoint/sleep_frame.bin.bak";

GfxRenderer patternedRenderer(const int width, const int height, const uint8_t seed) {
  GfxRenderer renderer(width, height);
  uint8_t* bytes = renderer.getFrameBuffer();
  for (size_t index = 0; index < renderer.getBufferSize(); ++index) {
    bytes[index] = static_cast<uint8_t>(seed + index * 17U);
  }
  return renderer;
}
}  // namespace

TEST(SleepFrameStoreTest, RoundTripsX3AndX4AndHonorsConsume) {
  for (const auto& [width, height] : std::vector<std::pair<int, int>>{{528, 792}, {480, 800}}) {
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    GfxRenderer source = patternedRenderer(width, height, 3);
    ASSERT_TRUE(SleepFrameStore::save(source));
    EXPECT_TRUE(SleepFrameStore::ready(width == 528));

    HalDisplay first(width, height);
    ASSERT_TRUE(SleepFrameStore::load(first, false));
    EXPECT_TRUE(std::equal(source.getFrameBuffer(), source.getFrameBuffer() + source.getBufferSize(),
                           first.getFrameBuffer()));
    EXPECT_TRUE(Storage.exists(FRAME));

    HalDisplay second(width, height);
    ASSERT_TRUE(SleepFrameStore::load(second));
    EXPECT_FALSE(Storage.exists(FRAME));
  }
}

TEST(SleepFrameStoreTest, RejectsSameSizeCorruptionAndLegacyRawFrames) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  GfxRenderer source = patternedRenderer(528, 792, 7);
  ASSERT_TRUE(SleepFrameStore::save(source));
  std::vector<uint8_t> corrupt = Storage.file(FRAME);
  corrupt.back() ^= 0x80U;
  Storage.setFile(FRAME, corrupt);
  EXPECT_FALSE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(FRAME));

  Storage.setFile(FRAME, std::vector<uint8_t>(source.getBufferSize(), 0xAA));
  EXPECT_FALSE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(FRAME));
}

TEST(SleepFrameStoreTest, RecoversValidBackupAndRejectsWrongModel) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  GfxRenderer source = patternedRenderer(528, 792, 11);
  ASSERT_TRUE(SleepFrameStore::save(source));
  const std::vector<uint8_t> valid = Storage.file(FRAME);
  std::vector<uint8_t> corrupt = valid;
  corrupt[corrupt.size() / 2] ^= 1U;
  Storage.setFile(FRAME, corrupt);
  Storage.setFile(BACKUP, valid);
  EXPECT_TRUE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_EQ(Storage.file(FRAME), valid);
  EXPECT_FALSE(SleepFrameStore::ready(false));
}
