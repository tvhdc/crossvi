#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "PerBookReaderSettingsCodec.h"
#include "network/SettingsApiUtils.h"

namespace {

using namespace PerBookReaderSettingsCodec;

PerBookReaderSettings populatedSettings() {
  PerBookReaderSettings settings;
  settings.hasReaderOverrides = true;
  settings.hasAutoPageTurnInterval = true;
  settings.autoPageTurnStartsOnOpen = true;
  settings.hasRenderModeOverride = true;
  settings.safeModeEnabled = true;
  settings.fontFamily = 1;
  settings.fontSize = 3;
  settings.lineSpacing = 2;
  settings.wordSpacing = 4;
  settings.paragraphAlignment = 4;
  settings.orientation = 3;
  settings.screenMargin = 40;
  settings.embeddedStyle = 0;
  settings.focusReadingEnabled = 1;
  settings.hyphenationEnabled = 1;
  settings.extraParagraphSpacing = 0;
  settings.textAntiAliasing = 0;
  settings.imageRendering = 2;
  settings.forceParagraphIndents = 1;
  settings.renderMode = EpubRenderMode::Full;
  settings.autoPageTurnSeconds = 120;
  setPerBookSdFontFamilyName(settings, "Noto Sans VN");
  return settings;
}

void refreshCrc(Encoded& encoded) {
  writeU32(encoded.data() + CRC_OFFSET, crc32(encoded.data() + PAYLOAD_OFFSET, PAYLOAD_SIZE));
}

std::vector<uint8_t> encodedAsVersion(const uint8_t version, const uint8_t fontSize, const uint8_t screenMargin = 40) {
  auto settings = populatedSettings();
  settings.fontSize = fontSize;
  settings.screenMargin = ReaderScreenMargin::closestValue(screenMargin);
  Encoded current{};
  EXPECT_TRUE(encode(settings, current));
  current[VERSION_OFFSET] = version;
  current[PAYLOAD_OFFSET + 6] = screenMargin;
  if (version <= AUTO_TURN_VERSION) {
    current[PAYLOAD_OFFSET] = version == LEGACY_VERSION ? 0x03U : 0x07U;
    writeU16(current.data() + PAYLOAD_LENGTH_OFFSET, LEGACY_PAYLOAD_SIZE);
    writeU32(current.data() + CRC_OFFSET, crc32(current.data() + PAYLOAD_OFFSET, LEGACY_PAYLOAD_SIZE));
    return {current.begin(), current.begin() + static_cast<std::ptrdiff_t>(LEGACY_ENCODED_SIZE)};
  }
  current[PAYLOAD_OFFSET + 48] = 0;
  writeU16(current.data() + PAYLOAD_LENGTH_OFFSET, PREVIOUS_PAYLOAD_SIZE);
  writeU32(current.data() + CRC_OFFSET, crc32(current.data() + PAYLOAD_OFFSET, PREVIOUS_PAYLOAD_SIZE));
  return {current.begin(), current.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_OFFSET + PREVIOUS_PAYLOAD_SIZE)};
}

}  // namespace

TEST(PerBookReaderSettingsCodec, RoundTripsAllFields) {
  const auto expected = populatedSettings();
  Encoded encoded;
  ASSERT_TRUE(encode(expected, encoded));

  PerBookReaderSettings decoded;
  EXPECT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
  EXPECT_EQ(decoded, expected);
}

TEST(PerBookReaderSettings, StoppingAutoTurnKeepsIntervalButDisablesRestart) {
  auto settings = populatedSettings();

  EXPECT_TRUE(setPerBookAutoPageTurnState(settings, 45, false));
  EXPECT_TRUE(settings.hasAutoPageTurnInterval);
  EXPECT_EQ(settings.autoPageTurnSeconds, 45);
  EXPECT_FALSE(settings.autoPageTurnStartsOnOpen);
  EXPECT_FALSE(setPerBookAutoPageTurnState(settings, 45, false));

  EXPECT_TRUE(setPerBookAutoPageTurnState(settings, 45, true));
  EXPECT_TRUE(settings.autoPageTurnStartsOnOpen);

  EXPECT_TRUE(setPerBookAutoPageTurnState(settings, 0, true));
  EXPECT_FALSE(settings.hasAutoPageTurnInterval);
  EXPECT_EQ(settings.autoPageTurnSeconds, 0);
  EXPECT_FALSE(settings.autoPageTurnStartsOnOpen);
}

TEST(PerBookReaderSettingsCodec, UsesStableExactByteLayout) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));

  const Encoded expected = {0x43, 0x56, 0x52, 0x53, 0x06, 0x31, 0x00, 0xAF, 0x2D, 0xB1, 0xF4, 0x1F, 0x01, 0x03, 0x02,
                            0x04, 0x03, 0x28, 0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0x78, 0x4E, 0x6F, 0x74, 0x6F, 0x20,
                            0x53, 0x61, 0x6E, 0x73, 0x20, 0x56, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04};
  EXPECT_EQ(encoded, expected);
}

TEST(ReaderScreenMargin, UsesTheExactSharedValuesAndSmallerTieBreak) {
  const std::array<uint8_t, 10> expected = {5, 10, 15, 20, 25, 30, 40, 50, 60, 70};
  EXPECT_EQ(ReaderScreenMargin::VALUES, expected);
  EXPECT_EQ(ReaderScreenMargin::closestValue(-1), 5);
  EXPECT_EQ(ReaderScreenMargin::closestValue(35), 30);
  EXPECT_EQ(ReaderScreenMargin::closestValue(36), 40);
  EXPECT_EQ(ReaderScreenMargin::closestValue(255), 70);
  EXPECT_EQ(ReaderScreenMargin::closestValue(std::numeric_limits<int>::min()), 5);
  EXPECT_EQ(ReaderScreenMargin::closestValue(std::numeric_limits<int>::max()), 70);
  for (uint8_t index = 0; index < ReaderScreenMargin::COUNT; ++index) {
    EXPECT_EQ(ReaderScreenMargin::closestIndex(ReaderScreenMargin::valueAt(index)), index);
    EXPECT_TRUE(ReaderScreenMargin::isValid(ReaderScreenMargin::valueAt(index)));
  }
}

TEST(SettingsApiUtils, ValidatesScreenMarginWebIndicesBeforeApplyingThem) {
  ASSERT_EQ(ReaderScreenMargin::COUNT, 10);

  EXPECT_TRUE(SettingsApiUtils::isValidEnumIndex(0, ReaderScreenMargin::COUNT));
  EXPECT_EQ(ReaderScreenMargin::valueAt(0), 5);

  EXPECT_TRUE(SettingsApiUtils::isValidEnumIndex(9, ReaderScreenMargin::COUNT));
  EXPECT_EQ(ReaderScreenMargin::valueAt(9), 70);

  EXPECT_FALSE(SettingsApiUtils::isValidEnumIndex(-1, ReaderScreenMargin::COUNT));
  EXPECT_FALSE(SettingsApiUtils::isValidEnumIndex(10, ReaderScreenMargin::COUNT));
}

TEST(PerBookReaderSettingsCodec, RejectsTruncationAndTrailingBytes) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  EXPECT_EQ(decode(encoded.data(), encoded.size() - 1, decoded), DecodeStatus::TRUNCATED);
  std::array<uint8_t, ENCODED_SIZE + 1> extended{};
  std::copy(encoded.begin(), encoded.end(), extended.begin());
  EXPECT_EQ(decode(extended.data(), extended.size(), decoded), DecodeStatus::WRONG_SIZE);
}

TEST(PerBookReaderSettingsCodec, RejectsCorruptHeaderAndPayload) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  auto corrupt = encoded;
  corrupt[0] ^= 0x01;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_MAGIC);

  corrupt = encoded;
  corrupt[VERSION_OFFSET] = VERSION + 1;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::NEWER_VERSION);

  corrupt = encoded;
  corrupt[VERSION_OFFSET] = 0;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::UNSUPPORTED_VERSION);

  corrupt = encoded;
  corrupt[PAYLOAD_LENGTH_OFFSET] = PAYLOAD_SIZE - 1;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_PAYLOAD_LENGTH);

  corrupt = encoded;
  corrupt[PAYLOAD_OFFSET + 1] ^= 0x01;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_CRC);
}

TEST(PerBookReaderSettingsCodec, RejectsOutOfRangeValuesWithValidCrc) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  const std::array<std::pair<size_t, uint8_t>, 18> invalidValues = {
      std::pair{size_t{0}, uint8_t{0x80}}, std::pair{size_t{1}, uint8_t{2}},  std::pair{size_t{2}, uint8_t{9}},
      std::pair{size_t{3}, uint8_t{3}},    std::pair{size_t{4}, uint8_t{5}},  std::pair{size_t{5}, uint8_t{4}},
      std::pair{size_t{6}, uint8_t{41}},   std::pair{size_t{7}, uint8_t{2}},  std::pair{size_t{8}, uint8_t{2}},
      std::pair{size_t{9}, uint8_t{2}},    std::pair{size_t{10}, uint8_t{2}}, std::pair{size_t{11}, uint8_t{2}},
      std::pair{size_t{12}, uint8_t{3}},   std::pair{size_t{13}, uint8_t{4}}, std::pair{size_t{45}, uint8_t{'x'}},
      std::pair{size_t{46}, uint8_t{2}},   std::pair{size_t{47}, uint8_t{3}}, std::pair{size_t{48}, uint8_t{5}},
  };
  for (const auto& [offset, value] : invalidValues) {
    auto corrupt = encoded;
    corrupt[PAYLOAD_OFFSET + offset] = value;
    refreshCrc(corrupt);
    EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::INVALID_VALUE) << offset;
  }

  auto invalid = populatedSettings();
  invalid.autoPageTurnSeconds = 4;
  EXPECT_FALSE(encode(invalid, encoded));

  invalid = populatedSettings();
  invalid.hasAutoPageTurnInterval = false;
  EXPECT_FALSE(encode(invalid, encoded));

  invalid = populatedSettings();
  invalid.hasRenderModeOverride = false;
  EXPECT_FALSE(encode(invalid, encoded));

  invalid = populatedSettings();
  invalid.sdFontFamilyName[0] = static_cast<char>(0xC0);
  invalid.sdFontFamilyName[1] = '\0';
  std::fill(invalid.sdFontFamilyName.begin() + 2, invalid.sdFontFamilyName.end(), '\0');
  EXPECT_FALSE(encode(invalid, encoded));

  auto corrupt = encoded;
  ASSERT_TRUE(encode(populatedSettings(), corrupt));
  corrupt[PAYLOAD_OFFSET + 14] = 0xC0;
  corrupt[PAYLOAD_OFFSET + 15] = 0;
  std::fill(corrupt.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_OFFSET + 16), corrupt.end(), 0);
  refreshCrc(corrupt);
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::INVALID_VALUE);
}

TEST(PerBookReaderSettingsCodec, RoundTripsEveryExtendedFontSizeWithoutChangingLegacyValues) {
  for (uint8_t fontSize = 0; fontSize < ReaderFontSize::COUNT; ++fontSize) {
    auto expected = populatedSettings();
    expected.fontSize = fontSize;
    Encoded encoded;
    ASSERT_TRUE(encode(expected, encoded)) << static_cast<int>(fontSize);
    PerBookReaderSettings decoded;
    ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
    EXPECT_EQ(decoded.fontSize, fontSize);
  }
}

TEST(PerBookReaderSettingsCodec, RoundTripsEveryScreenMarginValueInVersionFive) {
  for (const uint8_t margin : ReaderScreenMargin::VALUES) {
    auto expected = populatedSettings();
    expected.screenMargin = margin;
    Encoded encoded;
    ASSERT_TRUE(encode(expected, encoded)) << static_cast<int>(margin);
    PerBookReaderSettings decoded;
    ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
    EXPECT_EQ(decoded.screenMargin, margin);
  }

  for (const uint8_t invalid : {uint8_t{4}, uint8_t{35}, uint8_t{45}, uint8_t{55}, uint8_t{65}, uint8_t{71}}) {
    auto settings = populatedSettings();
    settings.screenMargin = invalid;
    Encoded encoded;
    EXPECT_FALSE(encode(settings, encoded)) << static_cast<int>(invalid);
  }

  Encoded malformed;
  ASSERT_TRUE(encode(populatedSettings(), malformed));
  malformed[PAYLOAD_OFFSET + 6] = 35;
  refreshCrc(malformed);
  PerBookReaderSettings decoded;
  EXPECT_EQ(decode(malformed.data(), malformed.size(), decoded), DecodeStatus::INVALID_VALUE);
}

TEST(PerBookReaderSettingsCodec, VersionsOneThroughThreeKeepTheFourSizeContract) {
  for (const uint8_t version : {LEGACY_VERSION, AUTO_TURN_VERSION, EPUB_OPTIONS_VERSION}) {
    for (uint8_t fontSize = 0; fontSize < ReaderFontSize::BUILTIN_COUNT; ++fontSize) {
      const auto encoded = encodedAsVersion(version, fontSize);
      PerBookReaderSettings decoded;
      ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK)
          << "version=" << static_cast<int>(version) << " size=" << static_cast<int>(fontSize);
      EXPECT_EQ(decoded.fontSize, fontSize);
    }
  }
}

TEST(PerBookReaderSettingsCodec, VersionThreeRejectsExtendedFontSizes) {
  for (uint8_t fontSize = ReaderFontSize::BUILTIN_COUNT; fontSize < ReaderFontSize::COUNT; ++fontSize) {
    const auto encoded = encodedAsVersion(EPUB_OPTIONS_VERSION, fontSize);
    PerBookReaderSettings decoded;
    EXPECT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::INVALID_VALUE)
        << static_cast<int>(fontSize);
  }
}

TEST(PerBookReaderSettingsCodec, VersionFourPreservesExtendedFontSizes) {
  for (uint8_t fontSize = 0; fontSize < ReaderFontSize::COUNT; ++fontSize) {
    const auto encoded = encodedAsVersion(EXTENDED_FONT_SIZE_VERSION, fontSize);
    PerBookReaderSettings decoded;
    ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK) << static_cast<int>(fontSize);
    EXPECT_EQ(decoded.fontSize, fontSize);
  }
}

TEST(PerBookReaderSettingsCodec, VersionsOneThroughFourNormalizeHistoricalMargins) {
  const std::array<std::pair<uint8_t, uint8_t>, 4> margins = {
      std::pair{uint8_t{5}, uint8_t{5}}, std::pair{uint8_t{30}, uint8_t{30}}, std::pair{uint8_t{35}, uint8_t{30}},
      std::pair{uint8_t{40}, uint8_t{40}}};
  for (const uint8_t version : {LEGACY_VERSION, AUTO_TURN_VERSION, EPUB_OPTIONS_VERSION, EXTENDED_FONT_SIZE_VERSION}) {
    for (const auto& [stored, expected] : margins) {
      const auto encoded = encodedAsVersion(version, 3, stored);
      PerBookReaderSettings decoded;
      ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK)
          << "version=" << static_cast<int>(version) << " margin=" << static_cast<int>(stored);
      EXPECT_EQ(decoded.screenMargin, expected);
    }
  }
}

TEST(PerBookReaderSettingsCodec, VersionFourRejectsMarginsOutsideItsHistoricalRange) {
  for (const uint8_t margin : {uint8_t{4}, uint8_t{50}}) {
    const auto encoded = encodedAsVersion(EXTENDED_FONT_SIZE_VERSION, 3, margin);
    PerBookReaderSettings decoded;
    EXPECT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::INVALID_VALUE) << static_cast<int>(margin);
  }
}

TEST(PerBookReaderSettingsCodec, VersionThreePreservesItsEpubOptions) {
  auto expected = populatedSettings();
  expected.wordSpacing = 0;
  const auto encoded = encodedAsVersion(EPUB_OPTIONS_VERSION, expected.fontSize);
  PerBookReaderSettings decoded;
  ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
  EXPECT_EQ(decoded, expected);
}

TEST(PerBookReaderSettingsCodec, ReadsVersionOneRatesAsSecondsAndKeepsLegacyAutoStart) {
  const std::array<std::pair<uint8_t, uint8_t>, 4> ratesToSeconds = {
      std::pair{uint8_t{1}, uint8_t{60}},
      std::pair{uint8_t{3}, uint8_t{20}},
      std::pair{uint8_t{6}, uint8_t{10}},
      std::pair{uint8_t{12}, uint8_t{5}},
  };
  for (const auto& [rate, seconds] : ratesToSeconds) {
    Encoded current{};
    ASSERT_TRUE(encode(populatedSettings(), current));
    std::array<uint8_t, LEGACY_ENCODED_SIZE> encoded{};
    std::copy_n(current.begin(), encoded.size(), encoded.begin());
    encoded[VERSION_OFFSET] = LEGACY_VERSION;
    writeU16(encoded.data() + PAYLOAD_LENGTH_OFFSET, LEGACY_PAYLOAD_SIZE);
    encoded[PAYLOAD_OFFSET] = 0x03;
    encoded[PAYLOAD_OFFSET + 13] = rate;
    writeU32(encoded.data() + CRC_OFFSET, crc32(encoded.data() + PAYLOAD_OFFSET, LEGACY_PAYLOAD_SIZE));

    PerBookReaderSettings decoded;
    ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK) << static_cast<int>(rate);
    EXPECT_TRUE(decoded.hasAutoPageTurnInterval);
    EXPECT_TRUE(decoded.autoPageTurnStartsOnOpen);
    EXPECT_EQ(decoded.autoPageTurnSeconds, seconds);
  }
}

TEST(PerBookReaderSettingsCodec, RejectsInvalidVersionOneRate) {
  Encoded current{};
  ASSERT_TRUE(encode(populatedSettings(), current));
  std::array<uint8_t, LEGACY_ENCODED_SIZE> encoded{};
  std::copy_n(current.begin(), encoded.size(), encoded.begin());
  encoded[VERSION_OFFSET] = LEGACY_VERSION;
  writeU16(encoded.data() + PAYLOAD_LENGTH_OFFSET, LEGACY_PAYLOAD_SIZE);
  encoded[PAYLOAD_OFFSET] = 0x03;
  encoded[PAYLOAD_OFFSET + 13] = 2;
  writeU32(encoded.data() + CRC_OFFSET, crc32(encoded.data() + PAYLOAD_OFFSET, LEGACY_PAYLOAD_SIZE));

  PerBookReaderSettings decoded;
  EXPECT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::INVALID_VALUE);
}

TEST(PerBookReaderSettingsCodec, DefaultsAndSdFontAreAlwaysTerminated) {
  PerBookReaderSettings defaults;
  EXPECT_FALSE(defaults.hasReaderOverrides);
  EXPECT_FALSE(defaults.hasAutoPageTurnInterval);
  EXPECT_FALSE(defaults.autoPageTurnStartsOnOpen);
  EXPECT_FALSE(defaults.hasRenderModeOverride);
  EXPECT_FALSE(defaults.safeModeEnabled);
  EXPECT_EQ(defaults.forceParagraphIndents, 0);
  EXPECT_EQ(defaults.renderMode, EpubRenderMode::Balanced);
  EXPECT_EQ(defaults.autoPageTurnSeconds, 0);
  EXPECT_EQ(defaults.sdFontFamilyName.front(), '\0');
  EXPECT_EQ(defaults.sdFontFamilyName.back(), '\0');

  setPerBookSdFontFamilyName(defaults, std::string(80, 'x'));
  EXPECT_EQ(defaults.sdFontFamilyName.back(), '\0');
  EXPECT_EQ(std::char_traits<char>::length(defaults.sdFontFamilyName.data()),
            PerBookReaderSettings::SD_FONT_NAME_CAPACITY - 1);

  Encoded encoded;
  ASSERT_TRUE(encode(defaults, encoded));
  PerBookReaderSettings decoded;
  ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
  EXPECT_EQ(decoded.sdFontFamilyName.back(), '\0');
}
