#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

struct SettingInfo {
  const char* key = nullptr;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  uint16_t CrossPointSettings::* value16Ptr = nullptr;
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;
  bool obfuscated = false;
  SettingType type = SettingType::ACTION;
  std::vector<uint8_t> enumValues;
  std::vector<std::string> enumStringValues;
  struct {
    uint16_t min = 0;
    uint16_t max = 0;
  } valueRange;
};

inline SettingInfo statusSetting(const char* key, uint8_t CrossPointSettings::* field, const SettingType type,
                                 const size_t optionCount = 0) {
  SettingInfo setting;
  setting.key = key;
  setting.valuePtr = field;
  setting.type = type;
  setting.enumValues.resize(optionCount);
  return setting;
}

inline const std::vector<SettingInfo>& getBaseSettingsList() {
  static const std::vector<SettingInfo> settings = {
      statusSetting("statusBarChapterPageCount", &CrossPointSettings::statusBarChapterPageCount, SettingType::TOGGLE),
      statusSetting("statusBarBookProgressPercentage", &CrossPointSettings::statusBarBookProgressPercentage,
                    SettingType::TOGGLE),
      statusSetting("statusBarProgressBar", &CrossPointSettings::statusBarProgressBar, SettingType::ENUM, 3),
      statusSetting("statusBarTitle", &CrossPointSettings::statusBarTitle, SettingType::ENUM, 3),
      statusSetting("statusBarBattery", &CrossPointSettings::statusBarBattery, SettingType::TOGGLE),
      statusSetting("outsideReaderClock", &CrossPointSettings::outsideReaderClock, SettingType::ENUM, 3),
      statusSetting("readerDarkMode", &CrossPointSettings::readerDarkMode, SettingType::TOGGLE),
  };
  return settings;
}

inline std::vector<SettingInfo> getSettingsList() { return getBaseSettingsList(); }
