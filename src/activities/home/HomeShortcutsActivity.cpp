#include "HomeShortcutsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "SettingsList.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/OpdsServerListActivity.h"
#include "activities/settings/SettingsSubmenuActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/settings/TimeSettingsActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void HomeShortcutsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  rebuildItems();
  requestUpdate();
}

void HomeShortcutsActivity::rebuildItems() {
  items_.clear();
  items_.reserve(SETTINGS.homeShortcuts.count);
  for (uint8_t index = 0; index < SETTINGS.homeShortcuts.count && index < HomeShortcutList::CAPACITY; ++index) {
    const uint8_t raw = SETTINGS.homeShortcuts.items[index];
    if (!isValidHomeShortcutId(raw)) continue;
    const auto id = static_cast<HomeShortcutId>(raw);
    if (isHomeShortcutAvailable(id, renderer)) items_.push_back(id);
  }
  selectedIndex_ = std::clamp(selectedIndex_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
}

void HomeShortcutsActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Home may need a different number of recent books after a layout change.
    // Replace it instead of revealing the stale instance under this activity.
    onGoHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (items_.empty()) return;

  const int count = static_cast<int>(items_.size());
  buttonNavigator_.onNext([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
}

const SettingInfo* HomeShortcutsActivity::findSetting(const char* key) const {
  if (!key) return nullptr;
  const auto& settings = getBaseSettingsList();
  const auto it = std::find_if(settings.begin(), settings.end(), [key](const SettingInfo& setting) {
    return setting.key && std::strcmp(setting.key, key) == 0;
  });
  return it == settings.end() ? nullptr : &*it;
}

void HomeShortcutsActivity::activateSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
  const HomeShortcutDescriptor* descriptor = findHomeShortcut(items_[selectedIndex_]);
  if (!descriptor) return;
  if (descriptor->target == HomeShortcutTarget::Setting) {
    activateSetting(*descriptor);
  } else {
    openScreen(descriptor->target);
  }
}

void HomeShortcutsActivity::activateSetting(const HomeShortcutDescriptor& descriptor) {
  const SettingInfo* setting = findSetting(descriptor.settingKey);
  if (!setting) return;

  if (setting->type == SettingType::TOGGLE && setting->valuePtr) {
    SETTINGS.*(setting->valuePtr) = !(SETTINGS.*(setting->valuePtr));
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (setting->type == SettingType::ENUM) {
    uint8_t current = setting->valueGetter ? setting->valueGetter()
                      : setting->valuePtr  ? SETTINGS.*(setting->valuePtr)
                                           : 0;
    auto onSelect = [this, valuePtr = setting->valuePtr, setter = setting->valueSetter](const int index) {
      if (setter) {
        setter(static_cast<uint8_t>(index));
      } else if (valuePtr) {
        SETTINGS.*valuePtr = static_cast<uint8_t>(index);
      }
      SETTINGS.saveToFile();
      requestUpdate();
    };
    if (!setting->enumStringValues.empty()) {
      optionPopup_.show(setting->nameId, setting->enumStringValues, current, std::move(onSelect));
    } else if (!setting->enumValues.empty()) {
      int optionCount = static_cast<int>(setting->enumValues.size());
      if (descriptor.id == HomeShortcutId::ParagraphAlignment && !SETTINGS.embeddedStyle &&
          optionCount == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) {
        // Match TextSettingsActivity: "Book style" has no source style to
        // follow while embedded styles are disabled.
        --optionCount;
        if (current == CrossPointSettings::BOOK_STYLE) current = CrossPointSettings::JUSTIFIED;
      }
      optionPopup_.show(setting->nameId, setting->enumValues.data(), optionCount, current, std::move(onSelect));
    }
    requestUpdate();
    return;
  }

  if (setting->type != SettingType::VALUE || (!setting->valuePtr && !setting->value16Ptr)) return;
  const bool is16Bit = setting->value16Ptr != nullptr;
  const int initial = is16Bit ? SETTINGS.*(setting->value16Ptr) : SETTINGS.*(setting->valuePtr);
  const bool sleepTimeout = setting->nameId == StrId::STR_TIME_TO_SLEEP;
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "HomeShortcutValue", setting->nameId, initial, setting->valueRange.min,
          setting->valueRange.max, setting->valueRange.step, sleepTimeout ? 5 : setting->valueRange.step,
          sleepTimeout ? StrId::STR_SLEEP_TIMER_VALUE_FORMAT : StrId::STR_NONE_OPT, false, true,
          sleepTimeout ? StrId::STR_SLEEP_NEVER : StrId::STR_NONE_OPT),
      [this, is16Bit, valuePtr = setting->valuePtr, value16Ptr = setting->value16Ptr](const ActivityResult& result) {
        if (!result.isCancelled) {
          const uint32_t value = std::get<IntervalResult>(result.data).value;
          if (is16Bit) {
            SETTINGS.*value16Ptr = static_cast<uint16_t>(value);
          } else {
            SETTINGS.*valuePtr = static_cast<uint8_t>(value);
          }
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void HomeShortcutsActivity::openScreen(const HomeShortcutTarget target) {
  std::unique_ptr<Activity> activity;
  switch (target) {
    case HomeShortcutTarget::Appearance:
      activity =
          std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, SettingsSubmenuActivity::Page::HomeLibrary);
      break;
    case HomeShortcutTarget::TextSettings:
      activity = std::make_unique<TextSettingsActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::StatusBar:
      activity = std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::Time:
      activity = std::make_unique<TimeSettingsActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::Language:
      activity = std::make_unique<LanguageSelectActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::FontManager:
      activity = std::make_unique<FontDownloadActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::WifiNetworks:
      activity = std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false);
      break;
    case HomeShortcutTarget::KOReaderSettings:
      activity = std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::OpdsServers:
      activity = std::make_unique<OpdsServerListActivity>(renderer, mappedInput);
      break;
    case HomeShortcutTarget::Setting:
      return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    SETTINGS.saveToFile();
    rebuildItems();
    requestUpdate();
  });
}

std::string HomeShortcutsActivity::valueLabel(const HomeShortcutId id) const {
  const HomeShortcutDescriptor* descriptor = findHomeShortcut(id);
  if (!descriptor || descriptor->target != HomeShortcutTarget::Setting) return {};
  const SettingInfo* setting = findSetting(descriptor->settingKey);
  if (!setting) return {};

  if (setting->type == SettingType::TOGGLE && setting->valuePtr) {
    if (id == HomeShortcutId::HideTxtBooks) {
      return I18N.get((SETTINGS.*(setting->valuePtr)) ? StrId::STR_STATE_OFF : StrId::STR_STATE_ON);
    }
    return I18N.get((SETTINGS.*(setting->valuePtr)) ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
  }
  if (setting->type == SettingType::ENUM) {
    const uint8_t value = setting->valueGetter ? setting->valueGetter()
                          : setting->valuePtr  ? SETTINGS.*(setting->valuePtr)
                                               : 0;
    if (!setting->enumStringValues.empty() && value < setting->enumStringValues.size()) {
      return setting->enumStringValues[value];
    }
    if (value < setting->enumValues.size()) return I18N.get(setting->enumValues[value]);
  }
  if (setting->type == SettingType::VALUE && setting->valuePtr) {
    if (setting->nameId == StrId::STR_TIME_TO_SLEEP &&
        SETTINGS.*(setting->valuePtr) >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return I18N.get(StrId::STR_SLEEP_NEVER);
    }
    if (setting->nameId == StrId::STR_TIME_TO_SLEEP) {
      char value[24]{};
      snprintf(value, sizeof(value), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned>(SETTINGS.*(setting->valuePtr)));
      return value;
    }
    return std::to_string(SETTINGS.*(setting->valuePtr));
  }
  if (setting->type == SettingType::VALUE && setting->value16Ptr) {
    return std::to_string(SETTINGS.*(setting->value16Ptr));
  }
  return {};
}

void HomeShortcutsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_SHORTCUTS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  if (items_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_NO_SHORTCUTS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, width, contentHeight}, static_cast<int>(items_.size()), selectedIndex_,
        [this](const int index) {
          const auto* descriptor = findHomeShortcut(items_[index]);
          return descriptor ? std::string(I18N.get(descriptor->label)) : std::string();
        },
        nullptr, nullptr, [this](const int index) { return valueLabel(items_[index]); }, true);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), items_.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
