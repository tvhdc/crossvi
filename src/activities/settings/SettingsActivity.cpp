#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Version.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DeviceInfoActivity.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "FontSizeSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TimeSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

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

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), dictionariesLoaded ? &dictionaries : nullptr)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      if ((setting.valuePtr == &CrossPointSettings::outsideReaderClock ||
           setting.valuePtr == &CrossPointSettings::showDateOutsideReader) &&
          !halClock.isAvailable()) {
        continue;
      }
      if (setting.valuePtr == &CrossPointSettings::showDateOutsideReader &&
          SETTINGS.outsideReaderClock == CrossPointSettings::STATUS_BAR_CLOCK_HIDE) {
        continue;
      }
      const bool quickResume =
          SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      if (quickResume && (setting.nameId == StrId::STR_SLEEP_SCREEN || setting.nameId == StrId::STR_SLEEP_COVER_MODE ||
                          setting.nameId == StrId::STR_SLEEP_COVER_FILTER)) {
        continue;
      }
      if ((setting.nameId == StrId::STR_SLEEP_COVER_MODE || setting.nameId == StrId::STR_SLEEP_COVER_FILTER) &&
          SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::COVER) {
        continue;
      }
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      if ((setting.nameId == StrId::STR_DICTIONARY_FONT || setting.nameId == StrId::STR_DICTIONARY_FONT_SIZE) &&
          dictionaries.empty()) {
        continue;
      }
      const bool supportsTextGrayscale = renderer.supportsStripGrayscale();
      if (!supportsTextGrayscale &&
          (setting.nameId == StrId::STR_TEXT_AA || setting.nameId == StrId::STR_TEXT_DARKNESS)) {
        continue;
      }
      if (setting.nameId == StrId::STR_TEXT_DARKNESS && (!SETTINGS.textAntiAliasing || SETTINGS.readerDarkMode)) {
        continue;
      }
      if (setting.nameId == StrId::STR_PARA_ALIGNMENT && !SETTINGS.embeddedStyle &&
          setting.enumValues.size() == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) {
        // "Book alignment" has no source style to follow while embedded styles
        // are disabled. Present its effective Justified value without changing
        // the saved preference, so re-enabling book formatting restores it.
        setting.enumValues.pop_back();
        setting.valuePtr = nullptr;
        setting.valueGetter = [] {
          return SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE
                     ? static_cast<uint8_t>(CrossPointSettings::JUSTIFIED)
                     : SETTINGS.paragraphAlignment;
        };
        setting.valueSetter = [](const uint8_t value) { SETTINGS.paragraphAlignment = value; };
      }
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  auto unorderedSystemSettings = std::move(systemSettings);
  systemSettings.clear();
  const auto appendSystemSetting = [this, &unorderedSystemSettings](const StrId id) {
    const auto it = std::find_if(unorderedSystemSettings.begin(), unorderedSystemSettings.end(),
                                 [id](const SettingInfo& setting) { return setting.nameId == id; });
    if (it == unorderedSystemSettings.end()) return;
    systemSettings.push_back(std::move(*it));
    unorderedSystemSettings.erase(it);
  };
  appendSystemSetting(StrId::STR_TIME_TO_SLEEP);
  if (halClock.isAvailable()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_TIME_SETTINGS, SettingAction::Time));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  appendSystemSetting(StrId::STR_DEVICE_DISPLAY_NAME);
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  appendSystemSetting(StrId::STR_SHOW_HIDDEN_FILES);
  // Preserve future persisted System settings even if their preferred order
  // has not yet been added above.
  for (auto& setting : unorderedSystemSettings) systemSettings.push_back(std::move(setting));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DEVICE_INFO, SettingAction::DeviceInfo));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  // Font installation is a maintenance action; keep it after the reading
  // preferences instead of displacing the common family/size controls.
  readerSettings.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));

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

void SettingsActivity::onExit() { Activity::onExit(); }

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
  if (mappedInput.isPressed(MappedInputManager::Button::Up) &&
      holdUp.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Up), 500)) {
    const bool rowWasSelected = selectedSettingIndex > 0;
    const int previousSetting = selectedSettingIndex;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    rebuildSettingsLists();
    selectedSettingIndex = rowWasSelected ? std::min(previousSetting, settingsCount) : 0;
    pendingNavigation = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Down) &&
      holdDown.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Down), 500)) {
    const bool rowWasSelected = selectedSettingIndex > 0;
    const int previousSetting = selectedSettingIndex;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    rebuildSettingsLists();
    selectedSettingIndex = rowWasSelected ? std::min(previousSetting, settingsCount) : 0;
    pendingNavigation = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (holdUp.onRelease() == ReaderUtils::HoldRelease::Short) --pendingNavigation;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (holdDown.onRelease() == ReaderUtils::HoldRelease::Short) ++pendingNavigation;
  }
  // Front buttons have no long-press action in Settings; keep them useful for
  // ordinary row navigation without allowing them to wrap into the tab bar.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) --pendingNavigation;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) ++pendingNavigation;

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
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.nameId == StrId::STR_FONT_SIZE) {
    startActivityForResult(std::make_unique<FontSizeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               rebuildSettingsLists();
                               return;
                             }
                             SETTINGS.saveToFile();
                             rebuildSettingsLists();
                           });
    return;
  }
  if (setting.type == SettingType::VALUE) {
    openValuePicker(setting);
    return;
  }

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
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
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
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
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

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openValuePicker(const SettingInfo& setting) {
  const bool is16Bit = setting.value16Ptr != nullptr;
  const int initialValue = is16Bit ? SETTINGS.*(setting.value16Ptr) : SETTINGS.*(setting.valuePtr);
  const auto valuePtr = setting.valuePtr;
  const auto value16Ptr = setting.value16Ptr;
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SettingsValueInterval", setting.nameId, initialValue, setting.valueRange.min,
          setting.valueRange.max, setting.valueRange.step, setting.valueRange.step, StrId::STR_NONE_OPT, false, true),
      [this, is16Bit, valuePtr, value16Ptr](const ActivityResult& result) {
        if (!result.isCancelled) {
          const uint32_t value = std::get<IntervalResult>(result.data).value;
          if (is16Bit) {
            SETTINGS.*value16Ptr = static_cast<uint16_t>(value);
          } else {
            SETTINGS.*valuePtr = static_cast<uint8_t>(value);
          }
          SETTINGS.saveToFile();
          rebuildSettingsLists();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
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
