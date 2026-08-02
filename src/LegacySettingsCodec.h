#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace LegacySettingsV2 {

constexpr uint8_t VERSION = 2;
constexpr uint8_t FIELD_COUNT = 30;
constexpr size_t MAX_ENCODED_BYTES = 294;

enum Field : uint8_t {
  SleepScreen = 0,
  ExtraParagraphSpacing = 1,
  ShortPowerButton = 2,
  StatusBar = 3,
  Orientation = 4,
  FrontButtonLayout = 5,
  SideButtonLayout = 6,
  FontFamily = 7,
  FontSize = 8,
  LineSpacing = 9,
  ParagraphAlignment = 10,
  SleepTimeout = 11,
  RefreshFrequency = 12,
  ScreenMargin = 13,
  SleepCoverMode = 14,
  OpdsUrl = 15,
  TextAntiAliasing = 16,
  HideBatteryPercentage = 17,
  LongPressButtonBehavior = 18,
  HyphenationEnabled = 19,
  OpdsUsername = 20,
  OpdsPassword = 21,
  SleepCoverFilter = 22,
  UiTheme = 23,
  FrontButtonBack = 24,
  FrontButtonConfirm = 25,
  FrontButtonLeft = 26,
  FrontButtonRight = 27,
  FadingFix = 28,
  EmbeddedStyle = 29,
};

struct Decoded {
  uint8_t count = 0;
  std::array<uint8_t, FIELD_COUNT> values{};

  bool has(const Field field) const { return static_cast<uint8_t>(field) < count; }
  uint8_t get(const Field field) const { return values[static_cast<size_t>(field)]; }
};

enum class DecodeStatus : uint8_t { Ok, Invalid, FutureVersion };

DecodeStatus decode(const uint8_t* data, size_t size, Decoded& output);

struct StatusBarValues {
  uint8_t chapterPageCount;
  uint8_t bookProgressPercentage;
  uint8_t progressBar;
  uint8_t title;
  uint8_t battery;
};

StatusBarValues statusBarValues(uint8_t legacyMode);

// Returns true only when a legacy value should be migrated. Modern canonical
// status-bar fields always win; a missing or invalid legacy value falls back
// to FULL (the historical default).
bool selectLegacyStatusBarMode(bool canonicalPresent, bool legacyPresent, int legacyMode, uint8_t& selectedMode);

}  // namespace LegacySettingsV2
