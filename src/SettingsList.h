#pragma once

#include <ClockDateFormat.h>
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReaderScreenMargin.h"
#include "activities/settings/SettingsActivity.h"
#include "util/DictionaryRegistry.h"

inline std::string readerFontSizeLabel(const uint8_t logicalSize) {
  return std::to_string(ReaderFontSize::pointSize(logicalSize)) + " pt";
}

inline SettingInfo buildPersistedFontSizeSetting() {
  std::vector<std::string> labels;
  labels.reserve(ReaderFontSize::COUNT);
  for (uint8_t index = 0; index < ReaderFontSize::COUNT; ++index) labels.push_back(readerFontSizeLabel(index));
  return SettingInfo::EnumStrings(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize, std::move(labels), "fontSize",
                                  StrId::STR_CAT_READER);
}

inline SettingInfo buildDictionaryFontSizeSetting() {
  std::vector<std::string> labels{I18N.get(StrId::STR_USE_READER_FONT_SIZE)};
  labels.reserve(ReaderFontSize::BUILTIN_COUNT + 1);
  for (uint8_t index = 0; index < ReaderFontSize::BUILTIN_COUNT; ++index) {
    labels.push_back(readerFontSizeLabel(index));
  }
  return SettingInfo::EnumStrings(StrId::STR_DICTIONARY_FONT_SIZE, &CrossPointSettings::dictionaryFontSize,
                                  std::move(labels), "dictionaryFontSize", StrId::STR_CAT_READER);
}

inline std::vector<std::string> dateFormatPatternLabels() {
  std::vector<std::string> labels;
  labels.reserve(ClockDateFormat::FormatCount);
  for (const char* pattern : ClockDateFormat::FORMAT_PATTERNS) labels.emplace_back(pattern);
  return labels;
}

inline SettingInfo buildScreenMarginSetting() {
  SettingInfo setting;
  setting.nameId = StrId::STR_SCREEN_MARGIN;
  setting.type = SettingType::ENUM;
  setting.key = "screenMargin";
  setting.category = StrId::STR_CAT_READER;
  setting.enumStringValues.reserve(ReaderScreenMargin::COUNT);
  for (const uint8_t value : ReaderScreenMargin::VALUES) {
    setting.enumStringValues.push_back(std::to_string(value));
  }
  setting.valueGetter = [] { return ReaderScreenMargin::closestIndex(SETTINGS.screenMargin); };
  setting.valueSetter = [](const uint8_t index) {
    if (index < ReaderScreenMargin::COUNT) SETTINGS.screenMargin = ReaderScreenMargin::valueAt(index);
  };
  return setting;
}

inline SettingInfo buildAvailableFontSizeSetting(const SdCardFontRegistry& registry) {
  std::vector<uint8_t> logicalSizes;
  const SdCardFontFamilyInfo* selectedFamily = nullptr;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    selectedFamily = registry.findFamily(SETTINGS.sdFontFamilyName);
    if (selectedFamily) {
      logicalSizes = selectedFamily->availableReaderSizeEnums();
    }
  }
  if (logicalSizes.empty()) {
    for (uint8_t index = 0; index < ReaderFontSize::BUILTIN_COUNT; ++index) logicalSizes.push_back(index);
  }

  SettingInfo setting;
  setting.nameId = StrId::STR_FONT_SIZE;
  setting.type = SettingType::ENUM;
  setting.key = "fontSize";
  setting.category = StrId::STR_CAT_READER;
  setting.enumStringValues.reserve(logicalSizes.size());
  for (const uint8_t logicalSize : logicalSizes) {
    const auto* file = selectedFamily ? selectedFamily->findClosestReaderSize(logicalSize) : nullptr;
    setting.enumStringValues.push_back(std::to_string(file ? file->pointSize : ReaderFontSize::pointSize(logicalSize)) +
                                       " pt");
  }
  setting.valueGetter = [logicalSizes] {
    const auto exact = std::find(logicalSizes.begin(), logicalSizes.end(), SETTINGS.fontSize);
    if (exact != logicalSizes.end()) return static_cast<uint8_t>(std::distance(logicalSizes.begin(), exact));

    const uint8_t target = ReaderFontSize::pointSize(SETTINGS.fontSize);
    uint8_t bestPosition = 0;
    uint8_t bestDelta = UINT8_MAX;
    for (uint8_t position = 0; position < logicalSizes.size(); ++position) {
      const uint8_t candidate = ReaderFontSize::pointSize(logicalSizes[position]);
      const uint8_t delta = candidate > target ? candidate - target : target - candidate;
      if (delta < bestDelta) {
        bestPosition = position;
        bestDelta = delta;
      }
    }
    return bestPosition;
  };
  setting.valueSetter = [logicalSizes](const uint8_t position) {
    if (position < logicalSizes.size()) SETTINGS.fontSize = logicalSizes[position];
  };
  return setting;
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SERIF};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SERIF));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the dictionary selection setting dynamically from the folders discovered
// under /dictionaries. "None" plus one option per dictionary; the selected folder
// name persists in SETTINGS.dictionaryName (saved/loaded manually in
// JsonSettingsIO — the generic loop skips dynamic entries).
inline SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [folderNames]() -> uint8_t {
    for (size_t i = 0; i < folderNames.size(); i++) {
      // Compare within the settings field capacity: an over-long folder name is
      // stored truncated, and must still match its list entry.
      if (strncmp(folderNames[i].c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;  // "None", also when the stored folder no longer exists
  };

  s.valueSetter = [folderNames](uint8_t v) {
    if (v == 0 || v > folderNames.size()) {
      SETTINGS.dictionaryName[0] = '\0';
      return;
    }
    strncpy(SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  };

  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
#ifdef SETTINGS_LIST_IMPLEMENTATION
const std::vector<SettingInfo>& getBaseSettingsList() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<StrId> statusBarClockValues(CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT);
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;

    std::vector<SettingInfo> v = {
        // --- Display ---
        SettingInfo::Enum(StrId::STR_HOME_LAYOUT, &CrossPointSettings::homeLayout,
                          {StrId::STR_HOME_LAYOUT_STYLE_1, StrId::STR_HOME_LAYOUT_STYLE_2,
                           StrId::STR_HOME_LAYOUT_STYLE_3, StrId::STR_HOME_LAYOUT_STYLE_4},
                          "homeLayout", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_SHOW_DEVICE_NAME_HOME, &CrossPointSettings::showDeviceNameOnHome,
                            "showDeviceNameOnHome", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_CLOCK_OUTSIDE_READER, &CrossPointSettings::outsideReaderClock,
                            "outsideReaderClock", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_DATE_OUTSIDE_READER, &CrossPointSettings::showDateOutsideReader,
                            "showDateOutsideReader", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_LIBRARY_DISPLAY_MODE, &CrossPointSettings::libraryView,
                          {StrId::STR_LIBRARY_LIST, StrId::STR_LIBRARY_COVERS}, "libraryView", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_LIBRARY_SORT, &CrossPointSettings::librarySort,
                          {StrId::STR_SORT_DATE_ADDED, StrId::STR_SORT_TITLE, StrId::STR_SORT_AUTHOR}, "librarySort",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_HIDE_TXT_BOOKS, &CrossPointSettings::hideTxtBooks, "hideTxtBooks",
                            StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_READ_BOOKS_IN_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                          {StrId::STR_KEEP, StrId::STR_AUTO_REMOVE}, "removeReadBooksFromRecents",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_QUICK_RESUME, &CrossPointSettings::quickResumeSleepScreen,
                            "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY),
        SettingInfo::DynamicEnum(
            StrId::STR_SLEEP_SCREEN,
            {StrId::STR_DEFAULT_VALUE, StrId::STR_COVER, StrId::STR_CUSTOM, StrId::STR_NONE_OPT,
             StrId::STR_READING_STATS},
            [] { return CrossPointSettings::sleepScreenSelection(SETTINGS.sleepScreen); },
            [](const uint8_t selection) { SETTINGS.sleepScreen = CrossPointSettings::sleepScreenMode(selection); },
            "sleepScreen", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(
            StrId::STR_SHOW_BATTERY_PERCENTAGE, &CrossPointSettings::hideBatteryPercentage,
            {StrId::STR_BATTERY_ALWAYS_SHOW, StrId::STR_BATTERY_HIDE_WHILE_READING, StrId::STR_BATTERY_ALWAYS_HIDE},
            "hideBatteryPercentage", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(
            StrId::STR_REFRESH_EVERY, &CrossPointSettings::refreshFrequency,
            {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
            "refreshFrequency", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY),

        // --- Reader ---
        // Built-in font-family entry. Replaced per-call with a registry-aware
        // version when SD fonts are installed.
        SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::STR_NOTO_SERIF},
                          "fontFamily", StrId::STR_CAT_READER),
        buildPersistedFontSizeSetting(),
        SettingInfo::Enum(StrId::STR_DICTIONARY_FONT, &CrossPointSettings::dictionaryFontFamily,
                          {StrId::STR_USE_READER_FONT, StrId::STR_NOTO_SERIF}, "dictionaryFontFamily",
                          StrId::STR_CAT_READER),
        buildDictionaryFontSizeSetting(),
        SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode, "readerDarkMode",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                          {StrId::STR_NORMAL, StrId::STR_WORD_SPACING_1, StrId::STR_WORD_SPACING_2,
                           StrId::STR_WORD_SPACING_3, StrId::STR_WORD_SPACING_4},
                          "wordSpacing", StrId::STR_CAT_READER),
        buildScreenMarginSetting(),
        SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                          {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                           StrId::STR_BOOK_S_STYLE},
                          "paragraphAlignment", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_EPUB_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_SKIP_EPUB_COVER_PAGE, &CrossPointSettings::skipEpubCoverPage,
                            "skipEpubCoverPage", StrId::STR_CAT_READER),
        SettingInfo::Enum(
            StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
            {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
            "orientation", StrId::STR_CAT_READER),
        // --- Controls ---
        SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED, StrId::STR_PAGE_TURN},
                          "sideButtonLayout", StrId::STR_CAT_CONTROLS),
        SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                            "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(
            StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES},
            "shortPwrBtn", StrId::STR_CAT_CONTROLS),
        SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_DOUBLE_POWER_READING, &CrossPointSettings::doublePowerReadingFunction,
                          {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION, StrId::STR_DICTIONARY,
                           StrId::STR_READING_STATS, StrId::STR_AUTO_PAGE_TURN, StrId::STR_ADD_HIGHLIGHT,
                           StrId::STR_SCREENSHOT_BUTTON},
                          "doublePowerReadingFunction", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU, &CrossPointSettings::longPressMenuFunction,
                          {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION, StrId::STR_DICTIONARY,
                           StrId::STR_READING_STATS, StrId::STR_AUTO_PAGE_TURN, StrId::STR_ADD_HIGHLIGHT,
                           StrId::STR_SCREENSHOT_BUTTON},
                          "longPressMenuFunction", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_DOUBLE_POWER_ACTION, &CrossPointSettings::doublePowerAction,
                          {StrId::STR_DISABLED, StrId::STR_DOUBLE_POWER_HOME, StrId::STR_DOUBLE_POWER_RESUME,
                           StrId::STR_DOUBLE_POWER_REFRESH, StrId::STR_SCREENSHOT_BUTTON},
                          "doublePowerAction", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_WHEN_LEAVING_READER, &CrossPointSettings::backShortToFileBrowser,
                          {StrId::STR_DESTINATION_HOME, StrId::STR_DESTINATION_FILE_BROWSER}, "backShortToFileBrowser",
                          StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_HOME_BACK_BUTTON, &CrossPointSettings::homeBackAction,
                          {StrId::STR_SHORTCUTS, StrId::STR_CONTINUE_READING, StrId::STR_NONE_OPT}, "homeBackAction",
                          StrId::STR_CAT_CONTROLS),

        // --- System ---
        SettingInfo::Value(
            StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
            {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
            "sleepTimeoutMinutes", StrId::STR_CAT_DISPLAY),
        SettingInfo::String(StrId::STR_DEVICE_DISPLAY_NAME, &SETTINGS.deviceDisplayName[0],
                            sizeof(SETTINGS.deviceDisplayName), "deviceDisplayName", StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM),

        // OPDS download folder: persisted + web-exposed, but category-less so it
        // is hidden from the on-device Settings screen (edited via OPDS UI).
        SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                            sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"),
        // OPDS download filename format: persisted + web-exposed, category-less so it
        // is hidden from the on-device Settings screen (cycled from the OPDS UI).
        SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                          {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                          "opdsFilenameFormat"),

        // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
              KOREADER_STORE.saveToFile();
            },
            "koUsername", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
              KOREADER_STORE.saveToFile();
            },
            "koPassword", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
            [](const std::string& v) {
              KOREADER_STORE.setServerUrl(v);
              KOREADER_STORE.saveToFile();
            },
            "koServerUrl", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
            [](uint8_t v) {
              KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
              KOREADER_STORE.saveToFile();
            },
            "koMatchMethod", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
            [](uint8_t v) {
              KOREADER_STORE.setSendMetadata(v != 0);
              KOREADER_STORE.saveToFile();
            },
            "koSendMetadata", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
            [](uint8_t v) {
              KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
              KOREADER_STORE.saveToFile();
            },
            "koSyncBehavior", StrId::STR_KOREADER_SYNC),
        // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
        SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
        // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
        SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, std::move(statusBarClockValues),
                          "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::EnumStrings(StrId::STR_DATE_FORMAT, &CrossPointSettings::dateFormat, dateFormatPatternLabels(),
                                 "dateFormat", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_DATE_SEPARATOR, &CrossPointSettings::dateSeparator,
            {StrId::STR_DATE_SEPARATOR_PERIOD, StrId::STR_DATE_SEPARATOR_HYPHEN, StrId::STR_DATE_SEPARATOR_SLASH},
            "dateSeparator", StrId::STR_CUSTOMISE_STATUS_BAR),
        // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
        // on next WiFi connect, which is useful when crossing time zones.
        SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
    };
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3)
    if (halTiltSensor.isAvailable()) {
      // Insert after the short power button setting (end of Controls section)
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
          v.insert(it + 1, SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                               "tiltPageTurn", StrId::STR_CAT_CONTROLS));
          break;
        }
      }
    }
    return v;
  }();

  return baseList;
}

std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry,
                                         const std::vector<DictionaryEntry>* dictionaries) {
  std::vector<SettingInfo> v = getBaseSettingsList();
  if (registry) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (registry->getFamilyCount() > 0 && it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
    it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (it != v.end()) *it = buildAvailableFontSizeSetting(*registry);
  }
  if (dictionaries && !dictionaries->empty()) {
    // Insert at the end of the Reader category (just before the first Controls entry).
    auto it =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.category == StrId::STR_CAT_CONTROLS; });
    v.insert(it, buildDictionarySetting(*dictionaries));
  }
  return v;
}
#else
const std::vector<SettingInfo>& getBaseSettingsList();
std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                         const std::vector<DictionaryEntry>* dictionaries = nullptr);
#endif
