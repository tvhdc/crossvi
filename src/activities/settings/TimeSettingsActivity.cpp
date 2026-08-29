#include "TimeSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <memory>
#include <string>

#include "ClockDateFormat.h"
#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace {
enum MenuItem { ITEM_FORMAT = 0, ITEM_UTC_OFFSET, ITEM_DATE_FORMAT, ITEM_DATE_SEPARATOR, ITEM_SYNC };

const StrId menuNames[TimeSettingsActivity::ITEM_COUNT] = {
    StrId::STR_CLOCK_FORMAT,   StrId::STR_CLOCK_UTC_OFFSET, StrId::STR_DATE_FORMAT,
    StrId::STR_DATE_SEPARATOR, StrId::STR_CLOCK_SYNC_NOW,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {
    StrId::STR_CLOCK_FORMAT_24H,
    StrId::STR_CLOCK_FORMAT_12H,
};

const StrId dateSeparatorNames[CrossPointSettings::DATE_SEPARATOR_COUNT] = {
    StrId::STR_DATE_SEPARATOR_PERIOD,
    StrId::STR_DATE_SEPARATOR_HYPHEN,
    StrId::STR_DATE_SEPARATOR_SLASH,
};

std::string formatUtcOffset(uint8_t biasedQuarterHours) {
  if (biasedQuarterHours > 104) biasedQuarterHours = 48;
  const int totalMinutes = (static_cast<int>(biasedQuarterHours) - 48) * 15;
  const int absoluteMinutes = totalMinutes < 0 ? -totalMinutes : totalMinutes;
  char value[16]{};
  snprintf(value, sizeof(value), "UTC%c%d:%02d", totalMinutes < 0 ? '-' : '+', absoluteMinutes / 60,
           absoluteMinutes % 60);
  return value;
}
}  // namespace

void TimeSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) SETTINGS.clockFormat = 0;
  if (SETTINGS.clockUtcOffsetQ > 104) SETTINGS.clockUtcOffsetQ = 48;
  if (SETTINGS.dateFormat >= CrossPointSettings::DATE_FORMAT_COUNT) {
    SETTINGS.dateFormat = CrossPointSettings::DATE_FORMAT_MONTH_DAY_YEAR_LONG;
  }
  if (SETTINGS.dateSeparator >= CrossPointSettings::DATE_SEPARATOR_COUNT) {
    SETTINGS.dateSeparator = CrossPointSettings::DATE_SEPARATOR_SLASH;
  }
  requestUpdate();
}

void TimeSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
}

void TimeSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_FORMAT:
      optionPopup.show(StrId::STR_CLOCK_FORMAT, clockFormatNames, CLOCK_FORMAT_ITEMS, SETTINGS.clockFormat,
                       [this](const int index) {
                         if (index == SETTINGS.clockFormat) return;
                         SETTINGS.clockFormat = static_cast<uint8_t>(index);
                         SETTINGS.saveToFile();
                       });
      requestUpdate();
      return;
    case ITEM_UTC_OFFSET:
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput),
                             [](const ActivityResult&) {});
      return;
    case ITEM_DATE_FORMAT:
      optionPopup.show(tr(STR_DATE_FORMAT), ClockDateFormat::FORMAT_PATTERNS, ClockDateFormat::FormatCount,
                       SETTINGS.dateFormat, [this](const int index) {
                         if (index == SETTINGS.dateFormat) return;
                         SETTINGS.dateFormat = static_cast<uint8_t>(index);
                         SETTINGS.saveToFile();
                       });
      requestUpdate();
      return;
    case ITEM_DATE_SEPARATOR:
      optionPopup.show(StrId::STR_DATE_SEPARATOR, dateSeparatorNames, CrossPointSettings::DATE_SEPARATOR_COUNT,
                       SETTINGS.dateSeparator, [this](const int index) {
                         if (index == SETTINGS.dateSeparator) return;
                         SETTINGS.dateSeparator = static_cast<uint8_t>(index);
                         SETTINGS.saveToFile();
                       });
      requestUpdate();
      return;
    case ITEM_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return;
    default:
      return;
  }
}

void TimeSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TIME_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, ITEM_COUNT, selectedIndex,
      [](const int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](const int index) {
        switch (index) {
          case ITEM_FORMAT:
            return std::string(
                I18N.get(clockFormatNames[SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0]));
          case ITEM_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_DATE_FORMAT:
            return std::string(ClockDateFormat::formatPattern(SETTINGS.dateFormat));
          case ITEM_DATE_SEPARATOR:
            return std::string(
                I18N.get(dateSeparatorNames[SETTINGS.dateSeparator < CrossPointSettings::DATE_SEPARATOR_COUNT
                                                ? SETTINGS.dateSeparator
                                                : CrossPointSettings::DATE_SEPARATOR_SLASH]));
          case ITEM_SYNC:
            return SETTINGS.clockHasBeenSynced ? std::string(tr(STR_CLOCK_SYNCED)) : std::string(tr(STR_NOT_SET));
          default:
            return std::string{};
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
