#include "CrossPointSettings.h"

#include <AtomicJsonFile.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>

#include "I18nKeys.h"
#include "LegacySettingsCodec.h"
#include "ReaderScreenMargin.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";

int builtInFontId(const uint8_t family, const uint8_t size) {
  const uint8_t builtinSize = std::min<uint8_t>(size, CrossPointSettings::EXTRA_LARGE);
  if (family == CrossPointSettings::NOTOSANS) {
    switch (builtinSize) {
      case CrossPointSettings::SMALL:
        return NOTOSANS_12_FONT_ID;
      case CrossPointSettings::MEDIUM:
        return NOTOSANS_14_FONT_ID;
      case CrossPointSettings::LARGE:
        return NOTOSANS_16_FONT_ID;
      case CrossPointSettings::EXTRA_LARGE:
      default:
        return NOTOSANS_18_FONT_ID;
    }
  }
  switch (builtinSize) {
    case CrossPointSettings::SMALL:
      return NOTOSERIF_12_FONT_ID;
    case CrossPointSettings::MEDIUM:
      return NOTOSERIF_14_FONT_ID;
    case CrossPointSettings::LARGE:
      return NOTOSERIF_16_FONT_ID;
    case CrossPointSettings::EXTRA_LARGE:
    default:
      return NOTOSERIF_18_FONT_ID;
  }
}

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

void applyLegacyStatusBar(CrossPointSettings& settings, const uint8_t mode) {
  const LegacySettingsV2::StatusBarValues values = LegacySettingsV2::statusBarValues(mode);
  settings.statusBarChapterPageCount = values.chapterPageCount;
  settings.statusBarBookProgressPercentage = values.bookProgressPercentage;
  settings.statusBarProgressBar = values.progressBar;
  settings.statusBarTitle = values.title;
  settings.statusBarBattery = values.battery;
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

bool CrossPointSettings::saveToFile() const {
  if (!persistenceWritable) return false;
  std::lock_guard<std::mutex> lock(_mutex);
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  std::string json;
  const AtomicFile::LoadStatus loaded = AtomicJsonFile::load(SETTINGS_FILE_JSON, json);
  if (loaded == AtomicFile::LoadStatus::Missing) {
    // Fall back to binary migration
    if (Storage.exists(SETTINGS_FILE_BIN)) {
      if (loadFromBinaryFile()) {
        migrateLanguageBinaryFile();
        if (saveToFile()) {
          if (Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK)) {
            LOG_DBG("CPS", "Migrated settings.bin to settings.json");
          } else {
            LOG_ERR("CPS", "Saved migrated JSON but could not archive settings.bin");
          }
          return true;
        } else {
          LOG_ERR("CPS", "Failed to save migrated settings to JSON");
          return false;
        }
      }
      persistenceWritable = false;
      return false;
    }

    // No settings files at all -- check for standalone language.bin
    persistenceWritable = true;
    return migrateLanguageBinaryFile();
  }
  if (loaded != AtomicFile::LoadStatus::Primary && loaded != AtomicFile::LoadStatus::Backup &&
      loaded != AtomicFile::LoadStatus::Temp) {
    persistenceWritable = false;
    LOG_ERR("CPS", "Could not recover a valid settings.json");
    return false;
  }
  bool resave = false;
  bool result;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
  }
  persistenceWritable = result;
  if (result && resave) {
    if (saveToFile()) {
      LOG_DBG("CPS", "Resaved settings to update format");
    } else {
      LOG_ERR("CPS", "Failed to resave settings after format update");
    }
  }
  migrateLanguageBinaryFile();
  return result;
}

bool CrossPointSettings::migrateLanguageBinaryFile() {
  const bool sourceExists = Storage.exists(LANG_FILE_BIN);
  const bool backupExists = Storage.exists(LANG_FILE_BAK);
  if (!sourceExists && !backupExists) return false;

  // language.bin stored only a numeric enum index. That index is ambiguous
  // across CrossPoint forks, so applying it can turn Vietnamese into Russian
  // after an upgrade. A previous CrossVi build may already have applied that
  // index and left language.bin.bak; reset only that exact auto-migrated value.
  const uint8_t previousLanguage = language;
  if (backupExists) {
    HalFile backup;
    std::array<uint8_t, 2> legacy{};
    if (Storage.openFileForRead("CPS", LANG_FILE_BAK, backup) && backup.fileSize64() == legacy.size() &&
        backup.read(legacy.data(), legacy.size()) == static_cast<int>(legacy.size()) && legacy[0] == 1 &&
        legacy[1] < V1_LANGUAGE_COUNT && language == static_cast<uint8_t>(V1_LANGUAGES[legacy[1]])) {
      language = static_cast<uint8_t>(Language::EN);
    }
  }

  // Keep a current selection that does not match the ambiguous backup, then
  // retire both numeric artifacts after publishing the stable ISO code.
  if (!saveToFile()) {
    language = previousLanguage;
    LOG_ERR("CPS", "Could not retire ambiguous language.bin because settings save failed");
    return false;
  }
  const bool removedSource = !sourceExists || Storage.remove(LANG_FILE_BIN);
  const bool removedBackup = !backupExists || Storage.remove(LANG_FILE_BAK);
  if (!removedSource || !removedBackup) LOG_ERR("CPS", "Saved language code but could not remove legacy artifact");
  LOG_DBG("CPS", "Retired ambiguous numeric language settings");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }
  const uint64_t fileSize = inputFile.fileSize64();
  if (fileSize < 2 || fileSize > LegacySettingsV2::MAX_ENCODED_BYTES) {
    inputFile.close();
    LOG_ERR("CPS", "Invalid legacy settings size: %llu", static_cast<unsigned long long>(fileSize));
    return false;
  }
  std::array<uint8_t, LegacySettingsV2::MAX_ENCODED_BYTES> bytes{};
  const bool read = inputFile.read(bytes.data(), static_cast<size_t>(fileSize)) == static_cast<int>(fileSize);
  const bool closed = inputFile.close();
  if (!read || !closed) {
    LOG_ERR("CPS", "Could not read legacy settings exactly");
    return false;
  }

  LegacySettingsV2::Decoded decoded;
  const LegacySettingsV2::DecodeStatus decodedStatus =
      LegacySettingsV2::decode(bytes.data(), static_cast<size_t>(fileSize), decoded);
  if (decodedStatus != LegacySettingsV2::DecodeStatus::Ok) {
    LOG_ERR("CPS", "Legacy settings decode failed (status %u)", static_cast<unsigned>(decodedStatus));
    return false;
  }

  std::lock_guard<std::mutex> lock(_mutex);
  const auto assignEnum = [&decoded](const LegacySettingsV2::Field field, uint8_t& destination, const uint8_t count) {
    if (decoded.has(field) && decoded.get(field) < count) destination = decoded.get(field);
  };
  const auto assignFlag = [&assignEnum](const LegacySettingsV2::Field field, uint8_t& destination) {
    assignEnum(field, destination, 2);
  };

  assignEnum(LegacySettingsV2::SleepScreen, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
  if (sleepScreen == QUICK_RESUME) {
    sleepScreen = DARK;
    quickResumeSleepScreen = QUICK_RESUME_AFTER_TIMEOUT;
  } else if (sleepScreen == COVER_CUSTOM) {
    sleepScreen = COVER;
  }
  assignFlag(LegacySettingsV2::ExtraParagraphSpacing, extraParagraphSpacing);
  assignEnum(LegacySettingsV2::ShortPowerButton, shortPwrBtn, SHORT_PWRBTN_COUNT);
  if (decoded.has(LegacySettingsV2::StatusBar) && decoded.get(LegacySettingsV2::StatusBar) < STATUS_BAR_MODE_COUNT) {
    statusBar = decoded.get(LegacySettingsV2::StatusBar);
    applyLegacyStatusBar(*this, statusBar);
  }
  assignEnum(LegacySettingsV2::Orientation, orientation, ORIENTATION_COUNT);
  assignEnum(LegacySettingsV2::FrontButtonLayout, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
  assignEnum(LegacySettingsV2::SideButtonLayout, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
  if (decoded.has(LegacySettingsV2::FontFamily)) {
    const uint8_t legacyFontFamily = decoded.get(LegacySettingsV2::FontFamily);
    if (legacyFontFamily < BUILTIN_FONT_COUNT) {
      fontFamily = legacyFontFamily;
    } else if (legacyFontFamily == LEGACY_OPENDYSLEXIC) {
      fontFamily = NOTOSERIF;
      strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
      sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
    }
  }
  assignEnum(LegacySettingsV2::FontSize, fontSize, EXTRA_LARGE + 1);
  assignEnum(LegacySettingsV2::LineSpacing, lineSpacing, LINE_COMPRESSION_COUNT);
  assignEnum(LegacySettingsV2::ParagraphAlignment, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
  if (decoded.has(LegacySettingsV2::SleepTimeout) &&
      decoded.get(LegacySettingsV2::SleepTimeout) < SLEEP_TIMEOUT_COUNT) {
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(decoded.get(LegacySettingsV2::SleepTimeout));
  }
  assignEnum(LegacySettingsV2::RefreshFrequency, refreshFrequency, REFRESH_FREQUENCY_COUNT);
  if (decoded.has(LegacySettingsV2::ScreenMargin)) {
    screenMargin = ReaderScreenMargin::closestValue(decoded.get(LegacySettingsV2::ScreenMargin));
  }
  assignEnum(LegacySettingsV2::SleepCoverMode, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
  assignFlag(LegacySettingsV2::TextAntiAliasing, textAntiAliasing);
  assignEnum(LegacySettingsV2::HideBatteryPercentage, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
  assignEnum(LegacySettingsV2::LongPressButtonBehavior, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
  assignFlag(LegacySettingsV2::HyphenationEnabled, hyphenationEnabled);
  assignEnum(LegacySettingsV2::SleepCoverFilter, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
  assignEnum(LegacySettingsV2::FrontButtonBack, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
  assignEnum(LegacySettingsV2::FrontButtonConfirm, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
  assignEnum(LegacySettingsV2::FrontButtonLeft, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
  assignEnum(LegacySettingsV2::FrontButtonRight, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
  assignFlag(LegacySettingsV2::FadingFix, fadingFix);
  assignFlag(LegacySettingsV2::EmbeddedStyle, embeddedStyle);

  if (decoded.has(LegacySettingsV2::FrontButtonRight)) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else if (decoded.has(LegacySettingsV2::FrontButtonLayout)) {
    applyLegacyFrontButtonLayout(*this);
  }

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  // SD card fonts use same compression as Bookerly (the most neutral values)
  if (sdFontFamilyName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
    }
  }

  switch (fontFamily) {
    case NOTOSERIF:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  return builtInFontId(fontFamily, fontSize);
}

int CrossPointSettings::getDictionaryFontId() const {
  if (dictionaryFontFamily == DICTIONARY_FONT_READER) return getReaderFontId();
  const uint8_t family = dictionaryFontFamily == DICTIONARY_FONT_NOTO_SANS ? NOTOSANS : NOTOSERIF;
  const uint8_t size = dictionaryFontSize == 0 ? std::min<uint8_t>(fontSize, EXTRA_LARGE)
                                               : std::min<uint8_t>(dictionaryFontSize - 1, EXTRA_LARGE);
  return builtInFontId(family, size);
}
