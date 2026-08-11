#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Version.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "OtaUpdateActivity.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
SettingInfo showTxtBooksSetting() {
  return SettingInfo::DynamicEnum(
      StrId::STR_SHOW_TXT_BOOKS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return SETTINGS.hideTxtBooks ? uint8_t{0} : uint8_t{1}; },
      [](const uint8_t value) { SETTINGS.hideTxtBooks = value ? 0 : 1; });
}

SettingInfo sleepScreenSetting() {
  return SettingInfo::DynamicEnum(
      StrId::STR_SLEEP_SCREEN,
      {StrId::STR_DEFAULT_VALUE, StrId::STR_COVER, StrId::STR_CUSTOM, StrId::STR_NONE_OPT, StrId::STR_READING_STATS,
       StrId::STR_COVER_WITH_STATS, StrId::STR_CUSTOM_WITH_STATS},
      [] { return CrossPointSettings::sleepScreenSelection(SETTINGS.sleepScreen); },
      [](const uint8_t value) { SETTINGS.sleepScreen = CrossPointSettings::sleepScreenMode(value); });
}
}  // namespace

StrId SettingsSubmenuActivity::title() const {
  switch (page_) {
    case Page::HomeLibrary:
      return StrId::STR_INTERFACE_CUSTOMIZATION;
    case Page::Sleep:
      return StrId::STR_SLEEP_SETTINGS;
    case Page::Dictionary:
      return StrId::STR_DICTIONARY;
    case Page::PageButtons:
      return StrId::STR_PAGE_TURN_BUTTONS;
    case Page::ConfirmButton:
      return StrId::STR_CONFIRM_BUTTON;
    case Page::PowerButton:
      return StrId::STR_POWER_BUTTON;
    case Page::FirmwareUpdate:
      return StrId::STR_FIRMWARE_UPDATES;
  }
  return StrId::STR_SETTINGS_TITLE;
}

void SettingsSubmenuActivity::rebuildSettings() {
  settings_.clear();
  switch (page_) {
    case Page::HomeLibrary:
      settings_.push_back(SettingInfo::Enum(StrId::STR_HOME_LAYOUT, &CrossPointSettings::homeLayout,
                                            {StrId::STR_HOME_LAYOUT_STYLE_1, StrId::STR_HOME_LAYOUT_STYLE_2,
                                             StrId::STR_HOME_LAYOUT_STYLE_3, StrId::STR_HOME_LAYOUT_STYLE_4}));
      settings_.push_back(SettingInfo::Enum(StrId::STR_LIBRARY_DISPLAY_MODE, &CrossPointSettings::libraryView,
                                            {StrId::STR_LIBRARY_LIST, StrId::STR_LIBRARY_COVERS}));
      settings_.push_back(
          SettingInfo::Enum(StrId::STR_LIBRARY_SORT, &CrossPointSettings::librarySort,
                            {StrId::STR_SORT_DATE_ADDED, StrId::STR_SORT_TITLE, StrId::STR_SORT_AUTHOR}));
      settings_.push_back(
          SettingInfo::Toggle(StrId::STR_SHOW_DEVICE_NAME_HOME, &CrossPointSettings::showDeviceNameOnHome));
      if (halClock.isAvailable()) {
        settings_.push_back(
            SettingInfo::Toggle(StrId::STR_CLOCK_OUTSIDE_READER, &CrossPointSettings::outsideReaderClock));
        settings_.push_back(
            SettingInfo::Toggle(StrId::STR_DATE_OUTSIDE_READER, &CrossPointSettings::showDateOutsideReader));
        if (SETTINGS.outsideReaderClock && SETTINGS.showDateOutsideReader) {
          settings_.push_back(SettingInfo::Enum(StrId::STR_OUTSIDE_READER_DATE_TIME_ORDER,
                                                &CrossPointSettings::outsideReaderDateTimeOrder,
                                                {StrId::STR_DATE_THEN_TIME, StrId::STR_TIME_THEN_DATE}));
        }
      }
      settings_.push_back(showTxtBooksSetting());
      settings_.push_back(SettingInfo::Enum(StrId::STR_READ_BOOKS_IN_RECENTS,
                                            &CrossPointSettings::removeReadBooksFromRecents,
                                            {StrId::STR_KEEP, StrId::STR_AUTO_REMOVE}));
      settings_.push_back(
          SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder));
      break;

    case Page::Sleep: {
      settings_.push_back(SettingInfo::Value(
          StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
          {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1}));
      settings_.push_back(SettingInfo::Toggle(StrId::STR_QUICK_RESUME, &CrossPointSettings::quickResumeSleepScreen));
      const bool quickResume =
          SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      if (!quickResume) {
        settings_.push_back(sleepScreenSetting());
        if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
            SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_STATS) {
          settings_.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                                {StrId::STR_FIT, StrId::STR_CROP}));
          settings_.push_back(
              SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED}));
        }
      }
      break;
    }

    case Page::Dictionary: {
      if (dictionaries_.empty()) break;
      SettingInfo dictionary = buildDictionarySetting(dictionaries_);
      dictionary.nameId = StrId::STR_DICTIONARY_SET;
      settings_.push_back(std::move(dictionary));
      settings_.push_back(SettingInfo::Enum(StrId::STR_TEXT_TAB_FONT, &CrossPointSettings::dictionaryFontFamily,
                                            {StrId::STR_USE_READER_FONT, StrId::STR_NOTO_SERIF}));
      SettingInfo size = buildDictionaryFontSizeSetting();
      size.nameId = StrId::STR_TEXT_TAB_SIZE;
      settings_.push_back(std::move(size));
      break;
    }

    case Page::PageButtons:
      settings_.push_back(
          SettingInfo::Enum(StrId::STR_BUTTON_LAYOUT_READING, &CrossPointSettings::sideButtonLayout,
                            {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED, StrId::STR_PAGE_TURN}));
      settings_.push_back(
          SettingInfo::Toggle(StrId::STR_FOLLOW_SCREEN_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation));
      settings_.push_back(SettingInfo::Enum(StrId::STR_HOLD_WHILE_READING, &CrossPointSettings::longPressButtonBehavior,
                                            {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                                             StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION}));
      break;

    case Page::ConfirmButton:
      settings_.push_back(SettingInfo::Enum(StrId::STR_HOLD_WHILE_READING, &CrossPointSettings::longPressMenuFunction,
                                            {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION,
                                             StrId::STR_DICTIONARY, StrId::STR_READING_STATS, StrId::STR_AUTO_PAGE_TURN,
                                             StrId::STR_ADD_HIGHLIGHT, StrId::STR_SCREENSHOT_BUTTON}));
      break;

    case Page::PowerButton:
      settings_.push_back(SettingInfo::Enum(
          StrId::STR_SINGLE_PRESS, &CrossPointSettings::shortPwrBtn,
          {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES}));
      if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) {
        settings_.push_back(SettingInfo::Enum(
            StrId::STR_DOUBLE_PRESS_READING, &CrossPointSettings::doublePowerReadingFunction,
            {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION, StrId::STR_DICTIONARY,
             StrId::STR_READING_STATS, StrId::STR_AUTO_PAGE_TURN, StrId::STR_ADD_HIGHLIGHT,
             StrId::STR_SCREENSHOT_BUTTON, StrId::STR_DOUBLE_POWER_REFRESH}));
        settings_.push_back(
            SettingInfo::Enum(StrId::STR_DOUBLE_PRESS_OUTSIDE_READER, &CrossPointSettings::doublePowerAction,
                              {StrId::STR_DISABLED, StrId::STR_DOUBLE_POWER_HOME, StrId::STR_DOUBLE_POWER_RESUME,
                               StrId::STR_DOUBLE_POWER_REFRESH, StrId::STR_SCREENSHOT_BUTTON}));
      }
      if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        settings_.push_back(
            SettingInfo::Toggle(StrId::STR_RETURN_FROM_FOOTNOTE, &CrossPointSettings::pwrBtnFootnoteBack));
      }
      break;

    case Page::FirmwareUpdate:
      settings_.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
      settings_.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
      break;
  }
  selectedIndex_ = std::clamp(selectedIndex_, 0, std::max(0, static_cast<int>(settings_.size()) - 1));
}

void SettingsSubmenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  rebuildSettings();
  requestUpdate();
}

void SettingsSubmenuActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }
  if (settings_.empty()) return;

  const int count = static_cast<int>(settings_.size());
  buttonNavigator_.onNextRelease([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onNextContinuous([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPreviousContinuous([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
}

void SettingsSubmenuActivity::handleSelection() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(settings_.size())) return;
  const SettingInfo& setting = settings_[selectedIndex_];

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
    SETTINGS.saveToFile();
    rebuildSettings();
    requestUpdate();
    return;
  }
  if (setting.type == SettingType::ENUM) {
    const uint8_t current = setting.valueGetter ? setting.valueGetter()
                            : setting.valuePtr  ? SETTINGS.*(setting.valuePtr)
                                                : 0;
    const auto select = [this, valuePtr = setting.valuePtr, setter = setting.valueSetter](const int index) {
      if (setter) {
        setter(static_cast<uint8_t>(index));
      } else if (valuePtr) {
        SETTINGS.*valuePtr = static_cast<uint8_t>(index);
      }
      SETTINGS.saveToFile();
      rebuildSettings();
      requestUpdate();
    };
    if (!setting.enumStringValues.empty()) {
      optionPopup_.show(setting.nameId, setting.enumStringValues, current, select);
    } else if (!setting.enumValues.empty()) {
      optionPopup_.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), current,
                        select);
    }
    requestUpdate();
    return;
  }
  if (setting.type == SettingType::ACTION) openAction(setting.action);
}

void SettingsSubmenuActivity::openSleepTimeoutPicker() {
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

void SettingsSubmenuActivity::openAction(const SettingAction action) {
  std::unique_ptr<Activity> activity;
  switch (action) {
    case SettingAction::CheckForUpdates:
      activity = std::make_unique<OtaUpdateActivity>(renderer, mappedInput);
      break;
    case SettingAction::SdFirmwareUpdate:
      activity = std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput);
      break;
    default:
      return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    SETTINGS.saveToFile();
    rebuildSettings();
    requestUpdate();
  });
}

std::string SettingsSubmenuActivity::valueLabel(const int index) const {
  const SettingInfo& setting = settings_[index];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    return I18N.get((SETTINGS.*(setting.valuePtr)) ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM) {
    const uint8_t value = setting.valueGetter ? setting.valueGetter()
                          : setting.valuePtr  ? SETTINGS.*(setting.valuePtr)
                                              : 0;
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) return I18N.get(setting.enumValues[value]);
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr) {
    if (SETTINGS.*(setting.valuePtr) >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return I18N.get(StrId::STR_SLEEP_NEVER);
    }
    char value[24]{};
    snprintf(value, sizeof(value), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
             static_cast<unsigned>(SETTINGS.*(setting.valuePtr)));
    return value;
  }
  return {};
}

void SettingsSubmenuActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, I18N.get(title()),
                 page_ == Page::FirmwareUpdate ? CROSSPOINT_VERSION : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  if (settings_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, width, contentHeight}, static_cast<int>(settings_.size()), selectedIndex_,
        [this](const int index) { return std::string(I18N.get(settings_[index].nameId)); }, nullptr, nullptr,
        [this](const int index) { return valueLabel(index); }, true);
  }

  const bool toggle = !settings_.empty() && settings_[selectedIndex_].type == SettingType::TOGGLE;
  const char* confirmLabel = settings_.empty() ? "" : toggle ? tr(STR_TOGGLE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
