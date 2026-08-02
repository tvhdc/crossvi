#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "JsonSettingsIO.h"
#include "LegacySettingsCodec.h"
#include "TestSupport.h"

namespace {

constexpr char SETTINGS_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_BIN_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char SETTINGS_JSON[] = "/.crosspoint/settings.json";

void appendUint32(std::vector<uint8_t>& bytes, const uint32_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> makeLegacy(const uint8_t count) {
  std::vector<uint8_t> bytes{LegacySettingsV2::VERSION, count};
  for (uint8_t field = 0; field < count; ++field) {
    if (field == LegacySettingsV2::OpdsUrl || field == LegacySettingsV2::OpdsUsername ||
        field == LegacySettingsV2::OpdsPassword) {
      appendUint32(bytes, 1);
      bytes.push_back('x');
    } else {
      bytes.push_back(0);
    }
  }
  return bytes;
}

void resetFakes() {
  Storage.reset();
  LegacySettingsTestSupport::resetAtomicJson();
}

void expectStatus(const LegacySettingsV2::StatusBarValues& expected) {
  EXPECT_EQ(SETTINGS.statusBarChapterPageCount, expected.chapterPageCount);
  EXPECT_EQ(SETTINGS.statusBarBookProgressPercentage, expected.bookProgressPercentage);
  EXPECT_EQ(SETTINGS.statusBarProgressBar, expected.progressBar);
  EXPECT_EQ(SETTINGS.statusBarTitle, expected.title);
  EXPECT_EQ(SETTINGS.statusBarBattery, expected.battery);
}

TEST(LegacySettingsMigrationIntegration, ValidBinaryPublishesJsonBeforeArchivingSource) {
  resetFakes();
  std::vector<uint8_t> bytes = makeLegacy(LegacySettingsV2::TextAntiAliasing + 1);
  bytes[2 + LegacySettingsV2::SleepScreen] = CrossPointSettings::LIGHT;
  // The three historical strings make field offsets variable.  The codec must
  // still apply this field after consuming the OPDS URL at index 15.
  bytes.back() = 1;
  Storage.setFile(SETTINGS_BIN, bytes);

  ASSERT_TRUE(SETTINGS.loadFromFile());
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::LIGHT);
  EXPECT_EQ(SETTINGS.textAntiAliasing, 1);
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 1);
  EXPECT_FALSE(LegacySettingsTestSupport::lastSavedJson().empty());
  EXPECT_TRUE(Storage.exists(SETTINGS_JSON));
  EXPECT_FALSE(Storage.exists(SETTINGS_BIN));
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN_BAK));
}

TEST(LegacySettingsMigrationIntegration, PublicationFailurePreservesLegacySource) {
  resetFakes();
  Storage.setFile(SETTINGS_BIN, makeLegacy(1));
  LegacySettingsTestSupport::failNextJsonSave();

  EXPECT_FALSE(SETTINGS.loadFromFile());
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 1);
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN));
  EXPECT_FALSE(Storage.exists(SETTINGS_BIN_BAK));
  EXPECT_FALSE(Storage.exists(SETTINGS_JSON));
}

TEST(LegacyStatusBarJsonIntegration, MigratesAllModesAndCanonicalFieldsWin) {
  for (int mode = 0; mode < CrossPointSettings::STATUS_BAR_MODE_COUNT; ++mode) {
    bool needsResave = false;
    const std::string json = "{\"statusBar\":" + std::to_string(mode) + "}";
    ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, json.c_str(), &needsResave));
    EXPECT_TRUE(needsResave);
    expectStatus(LegacySettingsV2::statusBarValues(static_cast<uint8_t>(mode)));
  }

  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS,
      R"({"statusBar":0,"statusBarChapterPageCount":1,"statusBarBookProgressPercentage":0,"statusBarProgressBar":1,"statusBarTitle":0,"statusBarBattery":1})",
      &needsResave));
  EXPECT_EQ(SETTINGS.statusBarChapterPageCount, 1);
  EXPECT_EQ(SETTINGS.statusBarBookProgressPercentage, 0);
  EXPECT_EQ(SETTINGS.statusBarProgressBar, 1);
  EXPECT_EQ(SETTINGS.statusBarTitle, 0);
  EXPECT_EQ(SETTINGS.statusBarBattery, 1);

  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, "{}", &needsResave));
  EXPECT_TRUE(needsResave);
  expectStatus(LegacySettingsV2::statusBarValues(CrossPointSettings::FULL));
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBar":99})", &needsResave));
  EXPECT_TRUE(needsResave);
  expectStatus(LegacySettingsV2::statusBarValues(CrossPointSettings::FULL));
}

TEST(SettingsJsonIntegration, PersistsReaderDarkModeAndOutsideClockPlacement) {
  bool needsResave = false;
  SETTINGS.outsideReaderClock = CrossPointSettings::STATUS_BAR_CLOCK_HIDE;
  SETTINGS.readerDarkMode = 0;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"outsideReaderClock":2,"readerDarkMode":1})", &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderClock, CrossPointSettings::STATUS_BAR_CLOCK_LEFT);
  EXPECT_EQ(SETTINGS.readerDarkMode, 1);

  SETTINGS.outsideReaderClock = CrossPointSettings::STATUS_BAR_CLOCK_HIDE;
  SETTINGS.readerDarkMode = 0;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"outsideReaderClock":99,"readerDarkMode":99})", &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderClock, CrossPointSettings::STATUS_BAR_CLOCK_HIDE);
  EXPECT_EQ(SETTINGS.readerDarkMode, 0);
}

TEST(SettingsJsonIntegration, MigratesLegacySleepChoicesToSeparateQuickResumeAndCanonicalCover) {
  bool needsResave = false;
  SETTINGS.sleepScreen = CrossPointSettings::DARK;
  SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_NEVER;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":6,"quickResumeSleepScreen":0})", &needsResave));
  EXPECT_TRUE(needsResave);
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::DARK);
  EXPECT_EQ(SETTINGS.quickResumeSleepScreen, CrossPointSettings::QUICK_RESUME_AFTER_TIMEOUT);

  needsResave = false;
  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":4})", &needsResave));
  EXPECT_TRUE(needsResave);
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::COVER);

  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::DARK),
            CrossPointSettings::SLEEP_SCREEN_DEFAULT);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::LIGHT),
            CrossPointSettings::SLEEP_SCREEN_DEFAULT);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::COVER_CUSTOM),
            CrossPointSettings::SLEEP_SCREEN_COVER);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_DEFAULT), CrossPointSettings::LIGHT);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_BLANK), CrossPointSettings::BLANK);
}

TEST(LegacySettingsMigrationIntegration, InvalidBinaryDoesNotPublishArchiveOrMutateSettings) {
  resetFakes();
  const uint8_t originalSleepScreen = SETTINGS.sleepScreen;
  std::vector<uint8_t> bytes = makeLegacy(1);
  bytes.pop_back();
  Storage.setFile(SETTINGS_BIN, bytes);

  EXPECT_FALSE(SETTINGS.loadFromFile());
  EXPECT_EQ(SETTINGS.sleepScreen, originalSleepScreen);
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 0);
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN));
  EXPECT_FALSE(Storage.exists(SETTINGS_BIN_BAK));

  resetFakes();
  bytes = makeLegacy(1);
  Storage.setFile(SETTINGS_BIN, bytes);
  Storage.shortReadFor(SETTINGS_BIN);
  EXPECT_FALSE(SETTINGS.loadFromFile());
  EXPECT_EQ(SETTINGS.sleepScreen, originalSleepScreen);
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 0);
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN));
  EXPECT_FALSE(Storage.exists(SETTINGS_BIN_BAK));

  resetFakes();
  bytes = makeLegacy(0);
  bytes[0] = LegacySettingsV2::VERSION + 1;
  Storage.setFile(SETTINGS_BIN, bytes);
  EXPECT_FALSE(SETTINGS.loadFromFile());
  EXPECT_EQ(SETTINGS.sleepScreen, originalSleepScreen);
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 0);
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN));
  EXPECT_FALSE(Storage.exists(SETTINGS_BIN_BAK));
}

}  // namespace
