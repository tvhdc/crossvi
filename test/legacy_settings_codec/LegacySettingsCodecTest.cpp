#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "LegacySettingsCodec.h"

namespace {

void appendUint32(std::vector<uint8_t>& bytes, const uint32_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> makeLegacy(const uint8_t count, const size_t urlBytes = 3, const size_t usernameBytes = 2,
                                const size_t passwordBytes = 1) {
  std::vector<uint8_t> bytes{LegacySettingsV2::VERSION, count};
  for (uint8_t field = 0; field < count; ++field) {
    size_t length = 0;
    if (field == LegacySettingsV2::OpdsUrl) length = urlBytes;
    if (field == LegacySettingsV2::OpdsUsername) length = usernameBytes;
    if (field == LegacySettingsV2::OpdsPassword) length = passwordBytes;
    if (field == LegacySettingsV2::OpdsUrl || field == LegacySettingsV2::OpdsUsername ||
        field == LegacySettingsV2::OpdsPassword) {
      appendUint32(bytes, static_cast<uint32_t>(length));
      bytes.insert(bytes.end(), length, static_cast<uint8_t>('x'));
    } else {
      bytes.push_back(static_cast<uint8_t>(field + 1));
    }
  }
  return bytes;
}

TEST(LegacySettingsCodec, AcceptsEveryHistoricalPrefixIncludingEmpty) {
  for (uint8_t count = 0; count <= LegacySettingsV2::FIELD_COUNT; ++count) {
    const std::vector<uint8_t> bytes = makeLegacy(count);
    LegacySettingsV2::Decoded decoded;
    ASSERT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Ok)
        << "count=" << static_cast<unsigned>(count);
    EXPECT_EQ(decoded.count, count);
  }
}

TEST(LegacySettingsCodec, ConsumesHistoricalOpdsStringsWithoutShiftingLaterFields) {
  const std::vector<uint8_t> bytes = makeLegacy(LegacySettingsV2::FIELD_COUNT, 127, 63, 63);
  LegacySettingsV2::Decoded decoded;
  ASSERT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Ok);
  EXPECT_EQ(decoded.get(LegacySettingsV2::TextAntiAliasing), LegacySettingsV2::TextAntiAliasing + 1);
  EXPECT_EQ(decoded.get(LegacySettingsV2::SleepCoverFilter), LegacySettingsV2::SleepCoverFilter + 1);
  EXPECT_EQ(decoded.get(LegacySettingsV2::EmbeddedStyle), LegacySettingsV2::EmbeddedStyle + 1);
}

TEST(LegacySettingsCodec, RejectsEveryTruncatedPrefixWithoutChangingOutput) {
  const std::vector<uint8_t> complete = makeLegacy(LegacySettingsV2::FIELD_COUNT);
  for (size_t size = 0; size < complete.size(); ++size) {
    LegacySettingsV2::Decoded decoded;
    decoded.count = 9;
    ASSERT_NE(LegacySettingsV2::decode(complete.data(), size, decoded), LegacySettingsV2::DecodeStatus::Ok)
        << "size=" << size;
    EXPECT_EQ(decoded.count, 9) << "size=" << size;
  }
}

TEST(LegacySettingsCodec, RejectsFutureVersionCountAndOversizedStrings) {
  auto bytes = makeLegacy(0);
  bytes[0] = LegacySettingsV2::VERSION + 1;
  LegacySettingsV2::Decoded decoded;
  EXPECT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded),
            LegacySettingsV2::DecodeStatus::FutureVersion);

  bytes = makeLegacy(0);
  bytes[1] = LegacySettingsV2::FIELD_COUNT + 1;
  EXPECT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Invalid);

  bytes = makeLegacy(LegacySettingsV2::OpdsUrl + 1, 128);
  EXPECT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Invalid);
  bytes = makeLegacy(LegacySettingsV2::OpdsUsername + 1, 0, 64);
  EXPECT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Invalid);
  bytes = makeLegacy(LegacySettingsV2::OpdsPassword + 1, 0, 0, 64);
  EXPECT_EQ(LegacySettingsV2::decode(bytes.data(), bytes.size(), decoded), LegacySettingsV2::DecodeStatus::Invalid);
}

TEST(LegacySettingsCodec, MapsAllLegacyStatusBarModes) {
  const LegacySettingsV2::StatusBarValues none = LegacySettingsV2::statusBarValues(0);
  EXPECT_EQ(none.chapterPageCount, 0);
  EXPECT_EQ(none.bookProgressPercentage, 0);
  EXPECT_EQ(none.progressBar, 2);
  EXPECT_EQ(none.title, 2);
  EXPECT_EQ(none.battery, 0);

  const LegacySettingsV2::StatusBarValues noProgress = LegacySettingsV2::statusBarValues(1);
  EXPECT_EQ(noProgress.title, 1);
  EXPECT_EQ(noProgress.battery, 1);

  const LegacySettingsV2::StatusBarValues full = LegacySettingsV2::statusBarValues(2);
  EXPECT_EQ(full.chapterPageCount, 1);
  EXPECT_EQ(full.bookProgressPercentage, 1);
  EXPECT_EQ(full.progressBar, 2);

  EXPECT_EQ(LegacySettingsV2::statusBarValues(3).progressBar, 0);
  EXPECT_EQ(LegacySettingsV2::statusBarValues(4).title, 2);
  EXPECT_EQ(LegacySettingsV2::statusBarValues(5).progressBar, 1);
  EXPECT_EQ(LegacySettingsV2::statusBarValues(255).bookProgressPercentage, 1);
}

TEST(LegacySettingsCodec, CanonicalStatusBarFieldsWinAndLegacyDefaultsAreSafe) {
  uint8_t selected = 99;
  EXPECT_FALSE(LegacySettingsV2::selectLegacyStatusBarMode(true, true, 0, selected));
  EXPECT_EQ(selected, 99);

  EXPECT_TRUE(LegacySettingsV2::selectLegacyStatusBarMode(false, false, 0, selected));
  EXPECT_EQ(selected, 2);
  EXPECT_TRUE(LegacySettingsV2::selectLegacyStatusBarMode(false, true, -1, selected));
  EXPECT_EQ(selected, 2);
  EXPECT_TRUE(LegacySettingsV2::selectLegacyStatusBarMode(false, true, 6, selected));
  EXPECT_EQ(selected, 2);

  for (int mode = 0; mode < 6; ++mode) {
    ASSERT_TRUE(LegacySettingsV2::selectLegacyStatusBarMode(false, true, mode, selected));
    EXPECT_EQ(selected, mode);
  }
}

}  // namespace
