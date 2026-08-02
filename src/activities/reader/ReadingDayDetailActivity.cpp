#include "ReadingDayDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
std::string formatDuration(const uint32_t seconds) {
  char value[24];
  if (seconds == 0) {
    snprintf(value, sizeof(value), "0m");
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "<1m");
  } else if (seconds < 3600) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  }
  return value;
}
}  // namespace

void ReadingDayDetailActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingDayDetailActivity::loop() {
  if (suppressInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease_ = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ReadingDayDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  char date[16] = "--/--/----";
  if (cell_.date.isValid()) {
    snprintf(date, sizeof(date), "%02u/%02u/%04u", static_cast<unsigned>(cell_.date.day),
             static_cast<unsigned>(cell_.date.month), static_cast<unsigned>(cell_.date.year));
  }
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_STATS_CALENDAR));
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, tr(STR_STATS_DAY_DETAIL),
                    date);

  const bool isLatest = snapshot_.hasLatestDayReadingSeconds && cell_.date.isValid() &&
                        readingStatsDayIndex(cell_.date) == snapshot_.latestReadingDay;
  const std::string duration = cell_.exactDuration ? formatDuration(cell_.readingSeconds) : "--";
  const std::string sessions = isLatest ? std::to_string(snapshot_.latestDaySessions) : "--";
  const char* quality = cell_.legacyDuration
                            ? tr(STR_STATS_DATA_LEGACY)
                            : (cell_.exactDuration ? tr(STR_STATS_DATA_COMPLETE) : tr(STR_STATS_UNAVAILABLE));
  constexpr std::array<StrId, 3> labels = {StrId::STR_STATS_READING_TIME, StrId::STR_STATS_SESSIONS,
                                           StrId::STR_STATS_DATA_QUALITY};
  const std::array<std::string, 3> values = {duration, sessions, quality};
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentBottom - contentTop)}, labels.size(), -1,
      [&labels](const int index) { return std::string(I18N.get(labels[static_cast<size_t>(index)])); },
      [&values](const int index) { return values[static_cast<size_t>(index)]; });

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
