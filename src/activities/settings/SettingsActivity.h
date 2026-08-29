#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryRegistry.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  Time,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  DeviceInfo,
  Appearance,
  TextSettings,
  SleepSettings,
  DictionarySettings,
  PageButtonSettings,
  ConfirmButtonSettings,
  PowerButtonSettings,
  FirmwareUpdates,
  SleepImagePosition,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  uint16_t CrossPointSettings::* value16Ptr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint16_t min;
    uint16_t max;
    uint16_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo EnumStrings(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values,
                                 const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumStringValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint16_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.value16Ptr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnumStrings(StrId nameId, std::vector<std::string> values, std::function<uint8_t()> getter,
                                        std::function<void(uint8_t)> setter) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumStringValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public Activity {
  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;
  bool showSaveError = false;
  int pendingNavigation = 0;
  ReaderUtils::HoldGestureState holdUp;
  ReaderUtils::HoldGestureState holdDown;
  ButtonNavigator buttonNavigator;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  std::vector<DictionaryEntry> dictionaries;
  bool dictionariesLoaded = false;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  OptionPopup optionPopup;

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  void toggleCurrentSetting();
  void rebuildSettingsLists();
  void releaseSettingsLists();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override;
};
