#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <TiltPageTurnPolicy.h>
#include <Version.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DeviceInfoActivity.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "SettingsSubmenuActivity.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "TimeSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // The Settings screen opens on Display. Delay SD directory traversal until
  // the Reader tab actually needs the dictionary selector.
  if (selectedCategoryIndex == 1 && !dictionariesLoaded) {
    DictionaryRegistry::discover(dictionaries);
    dictionariesLoaded = true;
  }

  displaySettings.push_back(SettingInfo::Action(StrId::STR_INTERFACE_CUSTOMIZATION, SettingAction::Appearance));
  displaySettings.push_back(SettingInfo::Action(StrId::STR_SLEEP_SETTINGS, SettingAction::SleepSettings));
  displaySettings.push_back(SettingInfo::Enum(
      StrId::STR_SHOW_BATTERY_PERCENTAGE, &CrossPointSettings::hideBatteryPercentage,
      {StrId::STR_BATTERY_ALWAYS_SHOW, StrId::STR_BATTERY_HIDE_WHILE_READING, StrId::STR_BATTERY_ALWAYS_HIDE}));
  displaySettings.push_back(SettingInfo::Enum(
      StrId::STR_REFRESH_EVERY, &CrossPointSettings::refreshFrequency,
      {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30}));
  displaySettings.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix));

  readerSettings.push_back(SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.push_back(SettingInfo::Enum(
      StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW}));
  readerSettings.push_back(
      SettingInfo::Enum(StrId::STR_EPUB_IMAGES, &CrossPointSettings::imageRendering,
                        {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS}));
  readerSettings.push_back(
      SettingInfo::Toggle(StrId::STR_SKIP_EPUB_COVER_PAGE, &CrossPointSettings::skipEpubCoverPage));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  if (!dictionaries.empty()) {
    readerSettings.push_back(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::DictionarySettings));
  }
  readerSettings.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));

  controlsSettings.push_back(SettingInfo::Action(StrId::STR_PAGE_TURN_BUTTONS, SettingAction::PageButtonSettings));
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_CONFIRM_BUTTON, SettingAction::ConfirmButtonSettings));
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_POWER_BUTTON, SettingAction::PowerButtonSettings));
  if (halTiltSensor.isAvailable()) {
    controlsSettings.push_back(SettingInfo::DynamicEnum(
        StrId::STR_TILT_SENSOR, {StrId::STR_DISABLED, StrId::STR_TILT_PAGE_TURN, StrId::STR_TILT_PAGE_TURN_REVERSED},
        [] { return TiltPageTurnPolicy::settingOptionForMode(SETTINGS.tiltPageTurn); },
        [](const uint8_t value) { SETTINGS.tiltPageTurn = TiltPageTurnPolicy::modeForSettingOption(value); }));
  }
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));

  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemSettings.push_back(SettingInfo::String(StrId::STR_DEVICE_DISPLAY_NAME, &SETTINGS.deviceDisplayName[0],
                                               sizeof(SETTINGS.deviceDisplayName)));
  if (halClock.isAvailable()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_TIME_SETTINGS, SettingAction::Time));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_FIRMWARE_UPDATES, SettingAction::FirmwareUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DEVICE_INFO, SettingAction::DeviceInfo));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::releaseSettingsLists() {
  std::vector<SettingInfo>().swap(displaySettings);
  std::vector<SettingInfo>().swap(readerSettings);
  std::vector<SettingInfo>().swap(controlsSettings);
  std::vector<SettingInfo>().swap(systemSettings);
  currentSettings = nullptr;
  settingsCount = 0;
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  pendingNavigation = 0;
  holdUp.reset();
  holdDown.reset();
  dictionaries.clear();
  dictionariesLoaded = false;

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

bool SettingsActivity::handleGlobalShortcut(const GlobalShortcut shortcut) {
  if (optionPopup.isActive()) return false;
  if (Storage.probeMedia() && !SETTINGS.saveToFile()) return false;
  return handleSafeGlobalShortcut(shortcut);
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Up/Down have a deliberate two-level contract on this screen: a short
  // press moves within the current category, while a hold switches category.
  // Recognise the hold before the release edge so the action fires at 500 ms
  // and the eventual release cannot also move a row.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) holdUp.onPress();
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) holdDown.onPress();
  if (mappedInput.isPressed(MappedInputManager::Button::Up)) {
    (void)holdUp.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Up), 500);
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Down)) {
    (void)holdDown.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Down), 500);
  }

  const auto moveCategory = [this](const int direction) {
    const bool rowWasSelected = selectedSettingIndex > 0;
    const int previousSetting = selectedSettingIndex;
    selectedCategoryIndex = direction < 0 ? ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount)
                                          : ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    rebuildSettingsLists();
    selectedSettingIndex = rowWasSelected ? std::min(previousSetting, settingsCount) : 0;
    pendingNavigation = 0;
    requestUpdate();
  };
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [&moveCategory] { moveCategory(-1); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [&moveCategory] { moveCategory(1); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (holdUp.onRelease() == ReaderUtils::HoldRelease::Short) --pendingNavigation;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (holdDown.onRelease() == ReaderUtils::HoldRelease::Short) ++pendingNavigation;
  }
  // Front buttons navigate rows immediately and repeat after the shared
  // 500 ms hold threshold, without wrapping into the tab bar.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) --pendingNavigation;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) ++pendingNavigation;
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] { --pendingNavigation; });
  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] { ++pendingNavigation; });

  if (pendingNavigation != 0) {
    while (pendingNavigation < 0) {
      if (selectedSettingIndex == 0) {
        selectedSettingIndex = settingsCount > 0 ? settingsCount : 0;
      } else {
        --selectedSettingIndex;
      }
      ++pendingNavigation;
    }
    while (pendingNavigation > 0) {
      if (selectedSettingIndex >= settingsCount) {
        selectedSettingIndex = 0;
      } else {
        ++selectedSettingIndex;
      }
      --pendingNavigation;
    }
    requestUpdate();
  }

  // Handle actions with early return
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
      rebuildSettingsLists();
    } else {
      toggleCurrentSetting();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      if (Storage.probeMedia()) SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (!setting.enumValues.empty()) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr](int idx) {
                         SETTINGS.*valuePtr = idx;
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
                       });
      requestUpdate();
      return;
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 0) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter](int idx) {
        valueSetter(idx);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
  } else if (setting.type == SettingType::STRING) {
    const std::string initialValue = setting.stringGetter
                                         ? setting.stringGetter()
                                         : std::string(reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset);
    const size_t stringOffset = setting.stringOffset;
    const size_t stringMaxLen = setting.stringMaxLen;
    const auto stringSetter = setting.stringSetter;
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(setting.nameId), initialValue,
                                                stringMaxLen > 0 ? stringMaxLen - 1 : 0, InputType::Text),
        [this, stringOffset, stringMaxLen, stringSetter](const ActivityResult& result) {
          if (result.isCancelled) return;
          const std::string& value = std::get<KeyboardResult>(result.data).text;
          if (stringSetter) {
            stringSetter(value);
          } else if (stringMaxLen > 0) {
            char* destination = reinterpret_cast<char*>(&SETTINGS) + stringOffset;
            std::strncpy(destination, value.c_str(), stringMaxLen - 1);
            destination[stringMaxLen - 1] = '\0';
          }
          SETTINGS.saveToFile();
          rebuildSettingsLists();
        });
    return;
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };
    const auto openSubmenu = [this](const SettingsSubmenuActivity::Page page,
                                    std::vector<DictionaryEntry> dictionaries) {
      releaseSettingsLists();
      startActivityForResult(
          std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, page, std::move(dictionaries)),
          [this](const ActivityResult&) {
            SETTINGS.saveToFile();
            rebuildSettingsLists();
          });
    };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Time:
        startActivityForResult(std::make_unique<TimeSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        // The parent Settings activity remains on the stack while the font
        // manager performs a TLS request. Release its copied setting rows so
        // mbedTLS has enough contiguous heap for GitHub's redirect headers.
        releaseSettingsLists();
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DeviceInfo:
        startActivityForResult(std::make_unique<DeviceInfoActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Appearance:
        openSubmenu(SettingsSubmenuActivity::Page::HomeLibrary, {});
        break;
      case SettingAction::TextSettings:
        releaseSettingsLists();
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::SleepSettings:
        openSubmenu(SettingsSubmenuActivity::Page::Sleep, {});
        break;
      case SettingAction::DictionarySettings:
        openSubmenu(SettingsSubmenuActivity::Page::Dictionary, dictionaries);
        break;
      case SettingAction::PageButtonSettings:
        openSubmenu(SettingsSubmenuActivity::Page::PageButtons, {});
        break;
      case SettingAction::ConfirmButtonSettings:
        openSubmenu(SettingsSubmenuActivity::Page::ConfirmButton, {});
        break;
      case SettingAction::PowerButtonSettings:
        openSubmenu(SettingsSubmenuActivity::Page::PowerButton, {});
        break;
      case SettingAction::FirmwareUpdates:
        openSubmenu(SettingsSubmenuActivity::Page::FirmwareUpdate, {});
        break;
      case SettingAction::CheckForUpdates:
      case SettingAction::SdFirmwareUpdate:
        // These actions belong to SettingsSubmenuActivity and are not exposed
        // directly by the four top-level tabs.
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  SETTINGS.saveToFile();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::array<TabInfo, categoryCount> tabs{};
  for (int i = 0; i < categoryCount; i++) {
    tabs[static_cast<size_t>(i)] = {I18N.get(categoryNames[i]), selectedCategoryIndex == i};
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE &&
                   (setting.valuePtr != nullptr || setting.value16Ptr != nullptr)) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = setting.value16Ptr ? std::to_string(SETTINGS.*(setting.value16Ptr))
                                           : std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::STRING) {
          valueText = setting.stringGetter
                          ? setting.stringGetter()
                          : std::string(reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset);
          if (valueText.empty()) valueText = tr(STR_NONE_OPT);
        } else if (setting.type == SettingType::ACTION && setting.nameId == StrId::STR_FIRMWARE_UPDATES &&
                   SETTINGS.availableOtaVersion[0] != '\0') {
          valueText = SETTINGS.availableOtaVersion;
        }
        return valueText;
      },
      true);

  // Draw help text
  const bool selectedSettingIsToggle =
      selectedSettingIndex > 0 && (*currentSettings)[selectedSettingIndex - 1].type == SettingType::TOGGLE;
  const char* confirmLabel =
      selectedSettingIndex == 0
          ? I18N.get(categoryNames[ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)])
      : selectedSettingIsToggle ? tr(STR_TOGGLE)
                                : tr(STR_SELECT);
  const char* backLabel = selectedSettingIndex == 0 ? tr(STR_HOME) : I18N.get(categoryNames[selectedCategoryIndex]);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
