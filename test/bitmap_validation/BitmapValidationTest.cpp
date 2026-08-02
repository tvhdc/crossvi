#include <Bitmap.h>
#include <HalStorage.h>
#include <StagedFileTransaction.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {
constexpr char FINAL_PATH[] = "/thumb.bmp";
constexpr char STAGING_PATH[] = "/thumb.bmp.tmp";
constexpr char BACKUP_PATH[] = "/thumb.bmp.bak";

void append16(std::vector<uint8_t>& output, const uint16_t value) {
  output.push_back(static_cast<uint8_t>(value));
  output.push_back(static_cast<uint8_t>(value >> 8U));
}

void append32(std::vector<uint8_t>& output, const uint32_t value) {
  output.push_back(static_cast<uint8_t>(value));
  output.push_back(static_cast<uint8_t>(value >> 8U));
  output.push_back(static_cast<uint8_t>(value >> 16U));
  output.push_back(static_cast<uint8_t>(value >> 24U));
}

void set32(std::vector<uint8_t>& output, const size_t offset, const uint32_t value) {
  ASSERT_LE(offset + 4, output.size());
  output[offset] = static_cast<uint8_t>(value);
  output[offset + 1] = static_cast<uint8_t>(value >> 8U);
  output[offset + 2] = static_cast<uint8_t>(value >> 16U);
  output[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> validBmp(const uint8_t pixel = 0x80U) {
  constexpr uint32_t pixelOffset = 14U + 40U + 8U;
  constexpr uint32_t fileSize = pixelOffset + 4U;
  std::vector<uint8_t> output;
  output.reserve(fileSize);
  append16(output, 0x4D42U);
  append32(output, fileSize);
  append32(output, 0);
  append32(output, pixelOffset);
  append32(output, 40);
  append32(output, 1);
  append32(output, 1);
  append16(output, 1);
  append16(output, 1);
  append32(output, 0);
  append32(output, 4);
  append32(output, 0);
  append32(output, 0);
  append32(output, 2);
  append32(output, 0);
  output.insert(output.end(), {0, 0, 0, 0, 255, 255, 255, 0});
  output.insert(output.end(), {pixel, 0, 0, 0});
  return output;
}

class BitmapValidationTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(BitmapValidationTest, AcceptsCompleteBitmap) {
  Storage.setFile(FINAL_PATH, validBmp());
  EXPECT_TRUE(Bitmap::validateFile(FINAL_PATH));
  EXPECT_EQ(Bitmap::inspectFile(FINAL_PATH), BitmapFileStatus::Valid);
}

TEST_F(BitmapValidationTest, DistinguishesMissingMalformedAndTransientIoFailure) {
  EXPECT_EQ(Bitmap::inspectFile(FINAL_PATH), BitmapFileStatus::Missing);

  Storage.setFile(FINAL_PATH, {0x42, 0x4D});
  EXPECT_EQ(Bitmap::inspectFile(FINAL_PATH), BitmapFileStatus::Invalid);

  Storage.setFile(FINAL_PATH, validBmp());
  Storage.makeUnreadable(FINAL_PATH);
  EXPECT_EQ(Bitmap::inspectFile(FINAL_PATH), BitmapFileStatus::IoError);
  Storage.makeReadable(FINAL_PATH);
  Storage.shortReadFor(FINAL_PATH);
  EXPECT_EQ(Bitmap::inspectFile(FINAL_PATH), BitmapFileStatus::IoError);
}

TEST_F(BitmapValidationTest, ValidDerivedBitmapRemainsReadyWhenStaleBackupCleanupFails) {
  Storage.setFile(FINAL_PATH, validBmp());
  Storage.setFile(BACKUP_PATH, validBmp(0x00U));
  Storage.failRemoveFor(BACKUP_PATH);

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::Ready);
  EXPECT_TRUE(Storage.exists(FINAL_PATH));
  EXPECT_TRUE(Storage.exists(BACKUP_PATH));
}

TEST_F(BitmapValidationTest, StructurallyInvalidDerivedBackupIsDiscardedForRegeneration) {
  Storage.setFile(BACKUP_PATH, {0x42U, 0x4DU});

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::Generate);
  EXPECT_FALSE(Storage.exists(BACKUP_PATH));
}

TEST_F(BitmapValidationTest, ValidDerivedBackupRestoresMissingFinal) {
  const auto backup = validBmp();
  Storage.setFile(BACKUP_PATH, backup);

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::Ready);
  EXPECT_EQ(Storage.file(FINAL_PATH), backup);
  EXPECT_FALSE(Storage.exists(BACKUP_PATH));
}

TEST_F(BitmapValidationTest, FailedInvalidBackupRemovalFailsClosed) {
  Storage.setFile(BACKUP_PATH, {0x42U, 0x4DU});
  Storage.failRemoveFor(BACKUP_PATH);

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::IoError);
  EXPECT_TRUE(Storage.exists(BACKUP_PATH));
  EXPECT_FALSE(Storage.exists(FINAL_PATH));
}

TEST_F(BitmapValidationTest, UnreadableFinalWithoutBackupFailsClosedWithoutMutation) {
  Storage.setFile(FINAL_PATH, validBmp());
  Storage.makeUnreadable(FINAL_PATH);

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::IoError);
  EXPECT_TRUE(Storage.exists(FINAL_PATH));
  EXPECT_FALSE(Storage.exists(BACKUP_PATH));
}

TEST_F(BitmapValidationTest, UnreadableDerivedBackupIsPreservedForRetry) {
  Storage.setFile(BACKUP_PATH, validBmp());
  Storage.makeUnreadable(BACKUP_PATH);

  EXPECT_EQ(Bitmap::inspectDerivedCache(FINAL_PATH), BitmapCacheState::IoError);
  EXPECT_TRUE(Storage.exists(BACKUP_PATH));
  EXPECT_FALSE(Storage.exists(FINAL_PATH));
}

TEST_F(BitmapValidationTest, RejectsEveryTruncatedHeader) {
  const auto valid = validBmp();
  for (size_t size = 0; size < 62; ++size) {
    Storage.setFile(FINAL_PATH, {valid.begin(), valid.begin() + size});
    EXPECT_FALSE(Bitmap::validateFile(FINAL_PATH)) << size;
  }
}

TEST_F(BitmapValidationTest, RejectsTruncatedPaletteAndPixelPayload) {
  auto truncatedPalette = validBmp();
  truncatedPalette.resize(60);
  Storage.setFile(FINAL_PATH, std::move(truncatedPalette));
  EXPECT_FALSE(Bitmap::validateFile(FINAL_PATH));

  auto truncatedPixels = validBmp();
  truncatedPixels.pop_back();
  Storage.setFile(FINAL_PATH, std::move(truncatedPixels));
  EXPECT_FALSE(Bitmap::validateFile(FINAL_PATH));
}

TEST_F(BitmapValidationTest, RejectsOutOfBoundsOffsetAndExtremeDimensions) {
  auto badOffset = validBmp();
  set32(badOffset, 10, UINT32_MAX);
  Storage.setFile(FINAL_PATH, std::move(badOffset));
  EXPECT_FALSE(Bitmap::validateFile(FINAL_PATH));

  auto hugeWidth = validBmp();
  set32(hugeWidth, 18, UINT32_MAX);
  Storage.setFile(FINAL_PATH, std::move(hugeWidth));
  EXPECT_FALSE(Bitmap::validateFile(FINAL_PATH));
}

TEST_F(BitmapValidationTest, InvalidStagingNeverReplacesValidThumbnail) {
  const auto oldBitmap = validBmp(0x80U);
  Storage.setFile(FINAL_PATH, oldBitmap);
  auto truncated = validBmp(0x00U);
  truncated.pop_back();
  Storage.setFile(STAGING_PATH, std::move(truncated));

  EXPECT_EQ(StagedFileTransaction::publish(FINAL_PATH, STAGING_PATH, BACKUP_PATH, Bitmap::validateFile),
            StagedFileTransaction::Status::InvalidStaging);
  EXPECT_EQ(Storage.file(FINAL_PATH), oldBitmap);
}

TEST_F(BitmapValidationTest, FailedPostPublishVerificationRestoresOldThumbnail) {
  const auto oldBitmap = validBmp(0x80U);
  Storage.setFile(FINAL_PATH, oldBitmap);
  Storage.setFile(STAGING_PATH, validBmp(0x00U));
  Storage.corruptRenameTo(FINAL_PATH);

  EXPECT_EQ(StagedFileTransaction::publish(FINAL_PATH, STAGING_PATH, BACKUP_PATH, Bitmap::validateFile),
            StagedFileTransaction::Status::IoError);
  EXPECT_EQ(Storage.file(FINAL_PATH), oldBitmap);
}
}  // namespace
