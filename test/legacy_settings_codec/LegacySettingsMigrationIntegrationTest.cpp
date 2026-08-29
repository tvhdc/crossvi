#include <HalStorage.h>
#include <I18nKeys.h>
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
constexpr char LANGUAGE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANGUAGE_BIN_BAK[] = "/.crosspoint/language.bin.bak";

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

TEST(LegacySettingsMigrationIntegration, BinaryAndLanguageMigrationPublishesJsonOnce) {
  resetFakes();
  Storage.setFile(SETTINGS_BIN, makeLegacy(1));
  Storage.setFile(LANGUAGE_BIN, {1, 6});

  ASSERT_TRUE(SETTINGS.loadFromFile());
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 1);
  EXPECT_TRUE(Storage.exists(SETTINGS_JSON));
  EXPECT_TRUE(Storage.exists(SETTINGS_BIN_BAK));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN));
}

TEST(LegacySettingsMigrationIntegration, RemovedBinaryNotoSansFallsBackToNotoSerif) {
  resetFakes();
  std::vector<uint8_t> bytes = makeLegacy(LegacySettingsV2::FontFamily + 1);
  bytes[2 + LegacySettingsV2::FontFamily] = CrossPointSettings::LEGACY_NOTOSANS;
  Storage.setFile(SETTINGS_BIN, bytes);

  ASSERT_TRUE(SETTINGS.loadFromFile());
  EXPECT_EQ(SETTINGS.fontFamily, CrossPointSettings::NOTOSERIF);
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 1);
  EXPECT_TRUE(Storage.exists(SETTINGS_JSON));
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

TEST(SettingsJsonIntegration, PersistsReaderDarkModeAndMigratesOutsideClockToToggle) {
  bool needsResave = false;
  SETTINGS.outsideReaderClock = 0;
  SETTINGS.readerDarkMode = 0;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"outsideReaderClock":2,"readerDarkMode":1})", &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderClock, 1);
  EXPECT_EQ(SETTINGS.readerDarkMode, 1);
  EXPECT_TRUE(needsResave);

  SETTINGS.outsideReaderClock = 0;
  SETTINGS.readerDarkMode = 0;
  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"outsideReaderClock":99,"readerDarkMode":99})", &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderClock, 0);
  EXPECT_EQ(SETTINGS.readerDarkMode, 0);
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, PersistsAndValidatesOutsideReaderDateTimeOrder) {
  resetFakes();
  SETTINGS.outsideReaderDateTimeOrder = CrossPointSettings::OUTSIDE_READER_TIME_THEN_DATE;
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.outsideReaderDateTimeOrder = CrossPointSettings::OUTSIDE_READER_DATE_THEN_TIME;
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderDateTimeOrder, CrossPointSettings::OUTSIDE_READER_TIME_THEN_DATE);
  EXPECT_FALSE(needsResave);

  SETTINGS.outsideReaderDateTimeOrder = CrossPointSettings::OUTSIDE_READER_DATE_THEN_TIME;
  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"outsideReaderDateTimeOrder":9})", &needsResave));
  EXPECT_EQ(SETTINGS.outsideReaderDateTimeOrder, CrossPointSettings::OUTSIDE_READER_DATE_THEN_TIME);
}

TEST(SettingsJsonIntegration, PreservesHistoricTiltDirectionAndRejectsInvalidModes) {
  resetFakes();
  SETTINGS.tiltPageTurn = CrossPointSettings::TILT_ON;
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.tiltPageTurn = CrossPointSettings::TILT_OFF;
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.tiltPageTurn, CrossPointSettings::TILT_ON);
  EXPECT_FALSE(needsResave);

  SETTINGS.tiltPageTurn = CrossPointSettings::TILT_INVERTED;
  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"tiltPageTurn":99})", &needsResave));
  EXPECT_EQ(SETTINGS.tiltPageTurn, CrossPointSettings::TILT_OFF);
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, PersistsAndValidatesVocabularySettings) {
  resetFakes();
  SETTINGS.vocabularyQuizSize = CrossPointSettings::VOCABULARY_QUIZ_30;
  SETTINGS.vocabularyQuestionTime = CrossPointSettings::VOCABULARY_TIME_UNLIMITED;
  SETTINGS.vocabularyAnswerCount = CrossPointSettings::VOCABULARY_ANSWERS_4;
  std::strcpy(SETTINGS.vocabularyDatasetPath, "/sets/travel.cvocab");
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.vocabularyQuizSize = CrossPointSettings::VOCABULARY_QUIZ_5;
  SETTINGS.vocabularyQuestionTime = CrossPointSettings::VOCABULARY_TIME_10_SECONDS;
  SETTINGS.vocabularyAnswerCount = CrossPointSettings::VOCABULARY_ANSWERS_3;
  SETTINGS.vocabularyDatasetPath[0] = '\0';
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.vocabularyQuizSize, CrossPointSettings::VOCABULARY_QUIZ_30);
  EXPECT_EQ(SETTINGS.vocabularyQuestionTime, CrossPointSettings::VOCABULARY_TIME_UNLIMITED);
  EXPECT_EQ(SETTINGS.vocabularyAnswerCount, CrossPointSettings::VOCABULARY_ANSWERS_4);
  EXPECT_STREQ(SETTINGS.vocabularyDatasetPath, "/sets/travel.cvocab");

  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS,
      R"({"vocabularyReaderPrompts":1,"vocabularyPromptFrequency":2,"vocabularyQuizSize":9,"vocabularyQuestionTime":9,"vocabularyAnswerCount":9})",
      &needsResave));
  EXPECT_EQ(SETTINGS.vocabularyQuizSize, CrossPointSettings::VOCABULARY_QUIZ_10);
  EXPECT_EQ(SETTINGS.vocabularyQuestionTime, CrossPointSettings::VOCABULARY_TIME_15_SECONDS);
  EXPECT_EQ(SETTINGS.vocabularyAnswerCount, CrossPointSettings::VOCABULARY_ANSWERS_3);
  EXPECT_TRUE(needsResave);

  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"vocabularyDatasetPath":"relative.cvocab"})", &needsResave));
  EXPECT_STREQ(SETTINGS.vocabularyDatasetPath, "");
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, PersistsLanguageByStableCodeAcrossReload) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::VI);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::VI));
}

TEST(SettingsJsonIntegration, RemovedLanguageFallsBackToEnglishAndIsCanonicalized) {
  bool needsResave = false;
  SETTINGS.language = static_cast<uint8_t>(Language::VI);

  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"language":"AR"})", &needsResave));

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::EN));
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, OversizeDictionaryNameIsClearedInsteadOfTruncated) {
  bool needsResave = false;
  const std::string name(32, 'd');
  const std::string json = "{\"dictionaryName\":\"" + name + "\"}";

  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, json.c_str(), &needsResave));

  EXPECT_STREQ(SETTINGS.dictionaryName, "");
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, RemovedLanguageAndLegacyIndexUpgradeSafelyToEnglish) {
  resetFakes();
  const std::string json = R"({"language":"RO"})";
  Storage.setFile(SETTINGS_JSON, std::vector<uint8_t>(json.begin(), json.end()));
  Storage.setFile(LANGUAGE_BIN_BAK, {1, 8});  // V1 index 8 was Romanian.

  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::EN));
  EXPECT_EQ(LegacySettingsTestSupport::jsonSaveCalls(), 1);
  EXPECT_NE(LegacySettingsTestSupport::lastSavedJson().find("\"language\":\"EN\""), std::string::npos);
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN_BAK));
}

TEST(SettingsJsonIntegration, PersistsValidatedOtaBadge) {
  resetFakes();
  std::strcpy(SETTINGS.availableOtaVersion, "1.0.2");
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.availableOtaVersion[0] = '\0';
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_STREQ(SETTINGS.availableOtaVersion, "1.0.2");
  EXPECT_FALSE(needsResave);

  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"availableOtaVersion":"1.0.1"})", &needsResave));
  EXPECT_STREQ(SETTINGS.availableOtaVersion, "");
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, RemovedNotoSansChoicesFallBackToNotoSerif) {
  resetFakes();
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"fontFamily":1,"dictionaryFontFamily":2,"sdFontFamilyName":""})", &needsResave));
  EXPECT_EQ(SETTINGS.fontFamily, CrossPointSettings::NOTOSERIF);
  EXPECT_EQ(SETTINGS.dictionaryFontFamily, CrossPointSettings::DICTIONARY_FONT_NOTO_SERIF);
  EXPECT_TRUE(needsResave);

  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"fontFamily":1,"sdFontFamilyName":"NotoSansVietnamese"})",
                                           &needsResave));
  EXPECT_EQ(SETTINGS.fontFamily, CrossPointSettings::NOTOSERIF);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "NotoSansVietnamese");
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, RemovesLegacyBackModesAndPreservesOrderedShortcuts) {
  resetFakes();
  SETTINGS.homeShortcuts.clear();
  ASSERT_TRUE(SETTINGS.homeShortcuts.add(HomeShortcutId::StatusBar));
  ASSERT_TRUE(SETTINGS.homeShortcuts.add(HomeShortcutId::TextSettings));
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.homeShortcuts.clear();
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.homeBackAction, CrossPointSettings::HOME_BACK_SHORTCUTS);
  EXPECT_EQ(SETTINGS.backShortToFileBrowser, 0);
  ASSERT_EQ(SETTINGS.homeShortcuts.count, 2);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(0), HomeShortcutId::StatusBar);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(1), HomeShortcutId::TextSettings);

  SETTINGS.homeBackAction = CrossPointSettings::HOME_BACK_NONE;
  SETTINGS.backShortToFileBrowser = 1;
  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"homeBackAction":1,"backShortToFileBrowser":1,"homeShortcuts":[5,36,1]})", &needsResave));
  EXPECT_EQ(SETTINGS.homeBackAction, CrossPointSettings::HOME_BACK_SHORTCUTS);
  EXPECT_EQ(SETTINGS.backShortToFileBrowser, 0);
  ASSERT_EQ(SETTINGS.homeShortcuts.count, 2);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(0), HomeShortcutId::StatusBar);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(1), HomeShortcutId::TextSettings);
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, RepairsInvalidDuplicateAndOversizedShortcutLists) {
  SETTINGS.homeShortcuts.clear();
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS, R"({"statusBarChapterPageCount":1,"homeShortcuts":[0,0,1,99,"bad",2,3,4,5,6,7,8,9]})", &needsResave));
  EXPECT_TRUE(needsResave);
  ASSERT_EQ(SETTINGS.homeShortcuts.count, HomeShortcutList::CAPACITY);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(0), HomeShortcutId::Appearance);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(1), HomeShortcutId::TextSettings);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(2), HomeShortcutId::QuickResume);
  EXPECT_EQ(SETTINGS.homeShortcuts.at(7), HomeShortcutId::ShowDeviceName);
}

TEST(SettingsJsonIntegration, StableLanguageCodeWinsOverAmbiguousLegacyIndex) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::VI);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));
  Storage.setFile(LANGUAGE_BIN, {1, 6});  // Index 6 means different languages in different forks.

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::VI));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN_BAK));
}

TEST(SettingsJsonIntegration, StandaloneAmbiguousLanguageIndexFallsBackToEnglish) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  Storage.setFile(LANGUAGE_BIN, {1, 6});

  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::EN));
  EXPECT_TRUE(Storage.exists(SETTINGS_JSON));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN_BAK));
}

TEST(SettingsJsonIntegration, RepairsLanguagePreviouslyAutoMigratedFromAmbiguousIndex) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::RU);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));
  Storage.setFile(LANGUAGE_BIN_BAK, {1, 6});

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::EN));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN_BAK));
}

TEST(SettingsJsonIntegration, ClosesLegacyLanguageBackupBeforeRemovingIt) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::RU);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));
  Storage.setFile(LANGUAGE_BIN_BAK, {1, 6});

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(Storage.removeWhileOpenAttemptsFor(LANGUAGE_BIN_BAK), 0U);
}

TEST(SettingsJsonIntegration, LanguageBackupCloseFailureLeavesMigrationRecoverable) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::RU);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));
  Storage.setFile(LANGUAGE_BIN_BAK, {1, 6});
  Storage.failCloseFor(LANGUAGE_BIN_BAK);

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  EXPECT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::RU));
  EXPECT_TRUE(Storage.exists(LANGUAGE_BIN_BAK));
  EXPECT_EQ(Storage.removeWhileOpenAttemptsFor(LANGUAGE_BIN_BAK), 0U);
}

TEST(SettingsJsonIntegration, PreservesExplicitLanguageThatDiffersFromAmbiguousBackup) {
  resetFakes();
  SETTINGS.language = static_cast<uint8_t>(Language::VI);
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));
  Storage.setFile(LANGUAGE_BIN_BAK, {1, 6});

  SETTINGS.language = static_cast<uint8_t>(Language::EN);
  ASSERT_TRUE(SETTINGS.loadFromFile());

  EXPECT_EQ(SETTINGS.language, static_cast<uint8_t>(Language::VI));
  EXPECT_FALSE(Storage.exists(LANGUAGE_BIN_BAK));
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
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::READING_CALENDAR),
            CrossPointSettings::SLEEP_SCREEN_READING_CALENDAR);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::COVER_STATS),
            CrossPointSettings::SLEEP_SCREEN_COVER_STATS);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::CUSTOM_STATS),
            CrossPointSettings::SLEEP_SCREEN_CUSTOM_STATS);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::CUSTOM),
            CrossPointSettings::SLEEP_SCREEN_CUSTOM);
  EXPECT_EQ(CrossPointSettings::sleepScreenSelection(CrossPointSettings::TRANSPARENT_CUSTOM),
            CrossPointSettings::SLEEP_SCREEN_CUSTOM);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_DEFAULT), CrossPointSettings::LIGHT);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_BLANK), CrossPointSettings::BLANK);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_READING_CALENDAR),
            CrossPointSettings::READING_CALENDAR);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_COVER_STATS),
            CrossPointSettings::COVER_STATS);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_CUSTOM_STATS),
            CrossPointSettings::CUSTOM_STATS);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_CUSTOM),
            CrossPointSettings::TRANSPARENT_CUSTOM);
  EXPECT_EQ(CrossPointSettings::sleepScreenMode(CrossPointSettings::SLEEP_SCREEN_TRANSPARENT),
            CrossPointSettings::TRANSPARENT_CUSTOM);

  needsResave = false;
  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":7})", &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::READING_CALENDAR);

  needsResave = false;
  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":8})", &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::COVER_STATS);

  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":9})", &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::CUSTOM_STATS);

  needsResave = false;
  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":10})", &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::TRANSPARENT_CUSTOM);

  needsResave = false;
  ASSERT_TRUE(
      JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepScreen":11})", &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::DARK);
  EXPECT_TRUE(needsResave);
}

TEST(SettingsJsonIntegration, PersistsAndClampsSleepGhostingTreatment) {
  resetFakes();
  SETTINGS.sleepGhostingTreatment = CrossPointSettings::SLEEP_GHOST_FULL_THREE_TIMES;
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.sleepGhostingTreatment = CrossPointSettings::SLEEP_GHOST_FULL_ONLY;
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.sleepGhostingTreatment, CrossPointSettings::SLEEP_GHOST_FULL_THREE_TIMES);

  SETTINGS.sleepGhostingTreatment = CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, R"({"statusBarChapterPageCount":1,"sleepGhostingTreatment":99})",
                                           &needsResave));
  EXPECT_EQ(SETTINGS.sleepGhostingTreatment, CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL);
}

TEST(SettingsJsonIntegration, PersistsAndClampsSleepImageTransform) {
  resetFakes();
  SETTINGS.sleepScreenImageZoom = 175;
  SETTINGS.sleepScreenImageOffsetX = -96;
  SETTINGS.sleepScreenImageOffsetY = 144;
  ASSERT_TRUE(JsonSettingsIO::saveSettings(SETTINGS, SETTINGS_JSON));

  SETTINGS.sleepScreenImageZoom = 100;
  SETTINGS.sleepScreenImageOffsetX = 0;
  SETTINGS.sleepScreenImageOffsetY = 0;
  bool needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(SETTINGS, LegacySettingsTestSupport::lastSavedJson().c_str(), &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreenImageZoom, 175);
  EXPECT_EQ(SETTINGS.sleepScreenImageOffsetX, -96);
  EXPECT_EQ(SETTINGS.sleepScreenImageOffsetY, 144);

  needsResave = false;
  ASSERT_TRUE(JsonSettingsIO::loadSettings(
      SETTINGS,
      R"({"statusBarChapterPageCount":1,"sleepScreenImageZoom":1,"sleepScreenImageOffsetX":5000,"sleepScreenImageOffsetY":-5000})",
      &needsResave));
  EXPECT_EQ(SETTINGS.sleepScreenImageZoom, 50);
  EXPECT_EQ(SETTINGS.sleepScreenImageOffsetX, 1024);
  EXPECT_EQ(SETTINGS.sleepScreenImageOffsetY, -1024);
  EXPECT_TRUE(needsResave);
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
