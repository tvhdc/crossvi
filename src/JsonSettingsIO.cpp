#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <AtomicJsonFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FontStorageUtils.h"
#include "LegacySettingsCodec.h"
#include "OpdsServerStore.h"
#include "ReaderScreenMargin.h"
#include "RecentBooksStore.h"
#include "SemanticVersion.h"
#include "SettingsList.h"
#include "Version.h"
#include "WifiCredentialStore.h"
#include "components/LibraryGridModel.h"

namespace {}  // namespace

// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  const LegacySettingsV2::StatusBarValues values = LegacySettingsV2::statusBarValues(settings.statusBar);
  settings.statusBarChapterPageCount = values.chapterPageCount;
  settings.statusBarBookProgressPercentage = values.bookProgressPercentage;
  settings.statusBarProgressBar = values.progressBar;
  settings.statusBarTitle = values.title;
  settings.statusBarBattery = values.battery;
}

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["showBootScreen"] = s.showBootScreen;

  String json;
  serializeJson(doc, json);
  const auto saved = AtomicJsonFile::save(path, json);
  return saved == AtomicFile::SaveStatus::Saved || saved == AtomicFile::SaveStatus::Unchanged;
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.showBootScreen = doc["showBootScreen"] | true;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  for (const auto& info : getBaseSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.value16Ptr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else if (info.value16Ptr) {
      doc[info.key] = s.*(info.value16Ptr);
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Keep a small marker so older per-tab/grid settings can be migrated without
  // changing the meaning of the shared setting on a later save.
  // The grid is fixed at 3x2 and is intentionally not user-facing, but keeping
  // the canonical value preserves compatibility with older settings files.
  doc["libraryGrid"] = s.libraryGrid;
  doc["libraryGridLayoutVersion"] = LibraryGridModel::LAYOUT_VERSION;
  doc["homeLayoutVersion"] = CrossPointSettings::HOME_LAYOUT_VERSION;
  // The UI remaps the two historic tilt directions into a user-facing
  // normal/reversed order, so this dynamic setting is persisted explicitly.
  doc["tiltPageTurn"] = s.tiltPageTurn;

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // Screen margin uses a dynamic enum so the UI and web API expose only the
  // supported non-uniform values. Persist the physical margin, not its index.
  doc["screenMargin"] = s.screenMargin;
  // Sleep screen also uses a dynamic enum so legacy numeric values can remain
  // readable while the UI exposes only the compact canonical choices.
  doc["sleepScreen"] = s.sleepScreen;
  doc["sleepScreenImageZoom"] = s.sleepScreenImageZoom;
  doc["sleepScreenImageOffsetX"] = s.sleepScreenImageOffsetX;
  doc["sleepScreenImageOffsetY"] = s.sleepScreenImageOffsetY;
  // SD card font family name — not in SettingsList, save manually
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (s.dictionaryName[0] != '\0') {
    doc["dictionaryName"] = s.dictionaryName;
  }
  if (s.availableOtaVersion[0] != '\0') doc["availableOtaVersion"] = s.availableOtaVersion;

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";
  doc["vocabularyQuizSize"] = s.vocabularyQuizSize;
  doc["vocabularyQuestionTime"] = s.vocabularyQuestionTime;
  doc["vocabularyAnswerCount"] = s.vocabularyAnswerCount;
  if (s.vocabularyDatasetPath[0] != '\0') doc["vocabularyDatasetPath"] = s.vocabularyDatasetPath;

  JsonArray shortcutArray = doc["homeShortcuts"].to<JsonArray>();
  for (uint8_t index = 0; index < s.homeShortcuts.count && index < HomeShortcutList::CAPACITY; ++index) {
    shortcutArray.add(s.homeShortcuts.items[index]);
  }

  String json;
  serializeJson(doc, json);
  const auto saved = AtomicJsonFile::save(path, json);
  return saved == AtomicFile::SaveStatus::Saved || saved == AtomicFile::SaveStatus::Unchanged;
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  uint8_t legacyStatusBarMode = CrossPointSettings::FULL;
  const bool canonicalStatusBarPresent = !doc["statusBarChapterPageCount"].isNull();
  const bool legacyStatusBarPresent = !doc["statusBar"].isNull();
  const int rawLegacyStatusBar = doc["statusBar"] | static_cast<int>(CrossPointSettings::FULL);
  if (LegacySettingsV2::selectLegacyStatusBarMode(canonicalStatusBarPresent, legacyStatusBarPresent, rawLegacyStatusBar,
                                                  legacyStatusBarMode)) {
    s.statusBar = legacyStatusBarMode;
    applyLegacyStatusBarSettings(s);
    if (needsResave) *needsResave = true;
  }
  // uiTheme was persisted by older firmware. It is intentionally ignored now
  // that CrossVi is the only UI, and removed on the next successful save.
  if (!doc["uiTheme"].isNull() && needsResave) *needsResave = true;

  // outsideReaderClock used to share the reader clock's hide/right/left enum.
  // Preserve either visible placement as the new boolean "shown on right".
  if (!doc["outsideReaderClock"].isNull()) {
    const int rawOutsideClock = doc["outsideReaderClock"] | 0;
    const uint8_t canonicalOutsideClock = rawOutsideClock == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT ||
                                                  rawOutsideClock == CrossPointSettings::STATUS_BAR_CLOCK_LEFT
                                              ? 1
                                              : 0;
    if (rawOutsideClock != canonicalOutsideClock && needsResave) *needsResave = true;
    doc["outsideReaderClock"] = canonicalOutsideClock;
  }

  for (const auto& info : getBaseSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.value16Ptr && !info.stringOffset) continue;

    if (info.value16Ptr) {
      const uint16_t fieldDefault = s.*(info.value16Ptr);
      const int raw = doc[info.key] | static_cast<int>(fieldDefault);
      s.*(info.value16Ptr) = static_cast<uint16_t>(
          std::clamp(raw, static_cast<int>(info.valueRange.min), static_cast<int>(info.valueRange.max)));
    } else if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        bool tooLong = false;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", info.stringMaxLen - 1, &ok,
                                                 &tooLong);
        if (tooLong && needsResave) *needsResave = true;
        if (!ok || val.empty()) {
          const char* legacyValue = doc[info.key] | fieldDefault.c_str();
          val = std::strlen(legacyValue) < info.stringMaxLen ? legacyValue : fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        const size_t optionCount =
            info.enumStringValues.empty() ? info.enumValues.size() : info.enumStringValues.size();
        v = clamp(v, static_cast<uint8_t>(optionCount), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (!doc["homeShortcuts"].isNull()) {
    if (!doc["homeShortcuts"].is<JsonArrayConst>()) {
      if (needsResave) *needsResave = true;
    } else {
      HomeShortcutList loaded;
      loaded.clear();
      for (JsonVariantConst item : doc["homeShortcuts"].as<JsonArrayConst>()) {
        if (!item.is<int>()) {
          if (needsResave) *needsResave = true;
          continue;
        }
        const int raw = item.as<int>();
        const auto shortcut = static_cast<HomeShortcutId>(raw);
        if (raw < 0 || raw > UINT8_MAX || !isValidHomeShortcutId(static_cast<uint8_t>(raw)) ||
            shortcut == HomeShortcutId::BackToFileBrowser || !loaded.add(shortcut)) {
          if (needsResave) *needsResave = true;
        }
      }
      s.homeShortcuts = loaded;
    }
  }

  // Value 2 meant Carousel before the three-cover layout was introduced.
  // Migrate it exactly once, then persist the version marker so the new Style
  // 3 keeps its canonical value on later boots.
  const int storedHomeLayout = doc["homeLayout"] | static_cast<int>(s.homeLayout);
  const uint8_t storedHomeLayoutVersion = doc["homeLayoutVersion"] | static_cast<uint8_t>(0);
  const uint8_t canonicalHomeLayout =
      CrossPointSettings::canonicalHomeLayout(storedHomeLayout, storedHomeLayoutVersion);
  if (s.homeLayout != canonicalHomeLayout && needsResave) *needsResave = true;
  s.homeLayout = canonicalHomeLayout;

  // Migrate the former per-tab fields. Prefer Recent because it was the
  // default tab; if it is absent, retain the old All value instead.
  if (doc["libraryView"].isNull()) {
    const bool hasRecent = !doc["recentLibraryView"].isNull();
    const int legacyView = hasRecent ? (doc["recentLibraryView"] | static_cast<int>(s.libraryView))
                                     : (doc["allLibraryView"] | static_cast<int>(s.libraryView));
    s.libraryView = static_cast<uint8_t>(std::clamp(legacyView, 0, CrossPointSettings::LIBRARY_VIEW_COUNT - 1));
    if ((hasRecent || !doc["allLibraryView"].isNull()) && needsResave) *needsResave = true;
  }

  const bool hasSharedGrid = !doc["libraryGrid"].isNull();
  const int rawGrid =
      hasSharedGrid ? (doc["libraryGrid"] | static_cast<int>(s.libraryGrid))
                    : (!doc["recentLibraryGrid"].isNull() ? (doc["recentLibraryGrid"] | static_cast<int>(s.libraryGrid))
                                                          : (doc["allLibraryGrid"] | static_cast<int>(s.libraryGrid)));
  const uint8_t canonicalGrid = LibraryGridModel::canonicalSetting(rawGrid);
  s.libraryGrid = canonicalGrid;
  const uint8_t storedGridVersion = doc["libraryGridLayoutVersion"] | static_cast<uint8_t>(0);
  if (!hasSharedGrid || storedGridVersion < LibraryGridModel::LAYOUT_VERSION || rawGrid != canonicalGrid) {
    if (needsResave) *needsResave = true;
  }

  // Screen margin is a dynamic enum and is therefore skipped by the generic
  // loop. Canonicalize legacy values such as 35 to the nearest supported value.
  const int storedScreenMargin = doc["screenMargin"] | static_cast<int>(s.screenMargin);
  s.screenMargin = ReaderScreenMargin::closestValue(storedScreenMargin);
  if (!doc["screenMargin"].isNull() && storedScreenMargin != s.screenMargin && needsResave) *needsResave = true;

  // Tilt page turning is also a dynamic enum. Preserve its historic stored
  // values so upgrading changes only the label, never the physical direction.
  const uint8_t storedTiltPageTurn = doc["tiltPageTurn"] | s.tiltPageTurn;
  s.tiltPageTurn = clamp(storedTiltPageTurn, CrossPointSettings::TILT_PAGE_TURN_COUNT, CrossPointSettings::TILT_OFF);
  if (!doc["tiltPageTurn"].isNull() && storedTiltPageTurn != s.tiltPageTurn && needsResave) *needsResave = true;

  // Keep old settings readable without exposing their redundant choices.
  // Quick Resume is now a separate switch, and Cover + Custom now uses the
  // single documented cover fallback (the bundled default screen).
  if (!doc["sleepScreen"].isNull()) {
    const int storedSleepScreen = doc["sleepScreen"] | static_cast<int>(s.sleepScreen);
    if (storedSleepScreen == CrossPointSettings::QUICK_RESUME) {
      s.sleepScreen = CrossPointSettings::DARK;
      s.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_AFTER_TIMEOUT;
      if (needsResave) *needsResave = true;
    } else if (storedSleepScreen == CrossPointSettings::COVER_CUSTOM) {
      s.sleepScreen = CrossPointSettings::COVER;
      if (needsResave) *needsResave = true;
    } else if (storedSleepScreen >= CrossPointSettings::DARK &&
               storedSleepScreen < CrossPointSettings::SLEEP_SCREEN_MODE_COUNT) {
      s.sleepScreen = static_cast<uint8_t>(storedSleepScreen);
    } else {
      s.sleepScreen = CrossPointSettings::DARK;
      if (needsResave) *needsResave = true;
    }
  }

  const auto loadSleepImageTransform = [&](const char* key, uint8_t& field, const int min, const int max) {
    const int raw = doc[key] | static_cast<int>(field);
    const int bounded = std::clamp(raw, min, max);
    field = static_cast<uint8_t>(bounded);
    if (!doc[key].isNull() && raw != bounded && needsResave) *needsResave = true;
  };
  loadSleepImageTransform("sleepScreenImageZoom", s.sleepScreenImageZoom, 50, 200);

  const auto loadSleepImageOffset = [&](const char* key, int16_t& field) {
    const int raw = doc[key] | static_cast<int>(field);
    const int bounded = std::clamp(raw, -1024, 1024);
    field = static_cast<int16_t>(bounded);
    if (!doc[key].isNull() && raw != bounded && needsResave) *needsResave = true;
  };
  loadSleepImageOffset("sleepScreenImageOffsetX", s.sleepScreenImageOffsetX);
  loadSleepImageOffset("sleepScreenImageOffsetY", s.sleepScreenImageOffsetY);

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)CrossPointSettings::SLEEP_10_MIN, CrossPointSettings::SLEEP_TIMEOUT_COUNT,
              (uint8_t)CrossPointSettings::SLEEP_10_MIN);
    s.sleepTimeoutMinutes = CrossPointSettings::sleepTimeoutEnumToMinutes(legacyValue);
    if (needsResave) *needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  s.fontFamily = clamp(storedFontFamily, CrossPointSettings::BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  if (!FontStorageUtils::copyPersistedFamilyName(sfn, s.sdFontFamilyName, sizeof(s.sdFontFamilyName))) {
    // Never persist or select a truncated family name: two long names could
    // otherwise collide after reboot.
    if (needsResave) *needsResave = true;
  }
  if (storedFontFamily == CrossPointSettings::LEGACY_OPENDYSLEXIC && s.sdFontFamilyName[0] == '\0') {
    s.fontFamily = CrossPointSettings::NOTOSERIF;
    strncpy(s.sdFontFamilyName, "OpenDyslexic", sizeof(s.sdFontFamilyName) - 1);
    s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
    if (needsResave) *needsResave = true;
  } else if (storedFontFamily >= CrossPointSettings::BUILTIN_FONT_COUNT) {
    if (needsResave) *needsResave = true;
  }

  // The removed built-in dictionary Noto Sans option must fall back to the
  // remaining built-in family, not to "follow reader" (which may be an SD font).
  const uint8_t storedDictionaryFontFamily =
      doc["dictionaryFontFamily"] | (uint8_t)CrossPointSettings::DICTIONARY_FONT_READER;
  if (storedDictionaryFontFamily == CrossPointSettings::LEGACY_DICTIONARY_FONT_NOTO_SANS) {
    s.dictionaryFontFamily = CrossPointSettings::DICTIONARY_FONT_NOTO_SERIF;
    if (needsResave) *needsResave = true;
  }

  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  const char* dictName = doc["dictionaryName"] | "";
  const size_t dictNameLength = std::strlen(dictName);
  if (dictNameLength < sizeof(s.dictionaryName)) {
    std::memcpy(s.dictionaryName, dictName, dictNameLength + 1);
  } else {
    // Truncating a persisted name can silently select a different dictionary
    // whose complete folder name happens to equal the truncated prefix.
    s.dictionaryName[0] = '\0';
    if (needsResave) *needsResave = true;
  }

  const char* otaVersion = doc["availableOtaVersion"] | "";
  if (std::strlen(otaVersion) < CrossPointSettings::OTA_VERSION_CAPACITY && ota_version::isValid(otaVersion) &&
      ota_version::isNewer(otaVersion, CROSSPOINT_VERSION)) {
    std::strncpy(s.availableOtaVersion, otaVersion, CrossPointSettings::OTA_VERSION_CAPACITY - 1);
    s.availableOtaVersion[CrossPointSettings::OTA_VERSION_CAPACITY - 1] = '\0';
  } else {
    s.availableOtaVersion[0] = '\0';
    if (otaVersion[0] != '\0' && needsResave) *needsResave = true;
  }

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    const char* storedLanguage = doc["language"].as<const char*>();
    s.language = static_cast<uint8_t>(I18n::languageFromCode(storedLanguage));
    if (std::strcmp(storedLanguage, LANGUAGE_CODES[s.language]) != 0 && needsResave) *needsResave = true;
  }
  // These controls were removed: Home Back always opens Shortcuts and a short
  // reader Back always returns Home. Drop stale values on the next save.
  if (!doc["homeBackAction"].isNull() || !doc["backShortToFileBrowser"].isNull()) {
    if (needsResave) *needsResave = true;
  }
  s.homeBackAction = CrossPointSettings::HOME_BACK_SHORTCUTS;
  s.backShortToFileBrowser = 0;

  // Reader vocabulary prompts were intentionally removed because they
  // interrupt reading. Preserve only explicit quiz settings.
  if (!doc["vocabularyReaderPrompts"].isNull() || !doc["vocabularyPromptFrequency"].isNull()) {
    if (needsResave) *needsResave = true;
  }
  s.vocabularyQuizSize = clamp(doc["vocabularyQuizSize"] | s.vocabularyQuizSize,
                               CrossPointSettings::VOCABULARY_QUIZ_SIZE_COUNT, CrossPointSettings::VOCABULARY_QUIZ_10);
  s.vocabularyQuestionTime =
      clamp(doc["vocabularyQuestionTime"] | s.vocabularyQuestionTime,
            CrossPointSettings::VOCABULARY_QUESTION_TIME_COUNT, CrossPointSettings::VOCABULARY_TIME_15_SECONDS);
  s.vocabularyAnswerCount =
      clamp(doc["vocabularyAnswerCount"] | s.vocabularyAnswerCount, CrossPointSettings::VOCABULARY_ANSWER_COUNT_COUNT,
            CrossPointSettings::VOCABULARY_ANSWERS_3);
  const char* vocabularyDatasetPath = doc["vocabularyDatasetPath"] | "";
  if (vocabularyDatasetPath[0] == '/' &&
      std::strlen(vocabularyDatasetPath) < CrossPointSettings::VOCABULARY_DATASET_PATH_CAPACITY) {
    std::strcpy(s.vocabularyDatasetPath, vocabularyDatasetPath);
  } else {
    s.vocabularyDatasetPath[0] = '\0';
    if (vocabularyDatasetPath[0] != '\0' && needsResave) *needsResave = true;
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}
