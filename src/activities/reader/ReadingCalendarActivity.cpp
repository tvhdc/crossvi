#include "ReadingCalendarActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#include "MappedInputManager.h"
#include "ReadingCalendarRenderer.h"
#include "ReadingDayDetailActivity.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void drawCentered(const GfxRenderer& renderer, const int fontId, const int x, const int width, const int y,
                  const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                  const bool black = true) {
  const int textWidth = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, x + std::max(0, (width - textWidth) / 2), y, text, black, style);
}

void drawMetric(const GfxRenderer& renderer, const Rect& rect, const std::string& value, const StrId label) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 6, true, true, true, true, true);
  const int valueHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int top = rect.y + std::max(2, (rect.height - valueHeight - labelHeight - 3) / 2);
  const std::string shownValue =
      renderer.truncatedText(UI_10_FONT_ID, value.c_str(), rect.width - 10, EpdFontFamily::BOLD);
  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, I18N.get(label), rect.width - 10);
  drawCentered(renderer, UI_10_FONT_ID, rect.x + 5, rect.width - 10, top, shownValue.c_str(), EpdFontFamily::BOLD);
  drawCentered(renderer, SMALL_FONT_ID, rect.x + 5, rect.width - 10, top + valueHeight + 3, shownLabel.c_str());
}

void drawSummary(const GfxRenderer& renderer, const Rect& rect, const ReadingCalendarMonthSummary& summary,
                 const uint8_t longestReadingStreak) {
  const int gap = 8;
  const int cellWidth = std::max(1, (rect.width - gap) / 2);
  const int cellHeight = std::max(1, (rect.height - gap) / 2);
  char count[16];
  snprintf(count, sizeof(count), "%u", static_cast<unsigned>(summary.readingDays));
  char streak[16];
  snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(longestReadingStreak));
  char best[32] = "--";
  if (summary.bestDayKnown) {
    const std::string duration = ReadingCalendarRenderer::formatDuration(summary.bestDaySeconds);
    snprintf(best, sizeof(best), "%s · %u", duration.c_str(), static_cast<unsigned>(summary.bestDayOfMonth));
  }
  drawMetric(renderer, Rect{rect.x, rect.y, cellWidth, cellHeight},
             ReadingCalendarRenderer::formatDuration(summary.totalSeconds, summary.totalIsMinimum),
             StrId::STR_STATS_MONTH_TOTAL);
  drawMetric(renderer, Rect{rect.x + cellWidth + gap, rect.y, rect.width - cellWidth - gap, cellHeight}, count,
             StrId::STR_STATS_MONTH_DAYS);
  drawMetric(renderer, Rect{rect.x, rect.y + cellHeight + gap, cellWidth, rect.height - cellHeight - gap}, best,
             StrId::STR_STATS_BEST_DAY);
  drawMetric(renderer,
             Rect{rect.x + cellWidth + gap, rect.y + cellHeight + gap, rect.width - cellWidth - gap,
                  rect.height - cellHeight - gap},
             streak, StrId::STR_STATS_LONGEST_STREAK);
}

}  // namespace

void ReadingCalendarActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  const DailyReadingHistory::LoadStatus status = DailyReadingHistory::load(dailyHistory_);
  const bool canRepair = status == DailyReadingHistory::LoadStatus::Ok ||
                         status == DailyReadingHistory::LoadStatus::Missing ||
                         status == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                         status == DailyReadingHistory::LoadStatus::RecoveredTemp;
  if (canRepair && model_.snapshot().hasLatestDayReadingSeconds) {
    const bool changed =
        dailyHistory_.reconcileExactDay(model_.snapshot().latestReadingDay, model_.snapshot().latestDayReadingSeconds);
    const bool recovered = status == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                           status == DailyReadingHistory::LoadStatus::RecoveredTemp;
    if ((changed || recovered) && !dailyHistory_.save()) {
      LOG_ERR("DAYHIST", "Could not persist reconciled latest reading day");
    }
  }
  model_.setDailyHistory(&dailyHistory_);
  requestUpdate();
}

void ReadingCalendarActivity::moveDay(const int delta) {
  bool changed = false;
  {
    RenderLock lock(*this);
    changed = model_.moveSelectedDay(delta);
  }
  if (changed) requestUpdate();
}

void ReadingCalendarActivity::moveMonth(const int delta) {
  bool changed = false;
  {
    RenderLock lock(*this);
    changed = delta < 0 ? model_.movePrevious() : model_.moveNext();
  }
  if (changed) requestUpdate();
}

void ReadingCalendarActivity::openSelectedDay() {
  if (!model_.isAvailable()) return;
  startActivityForResult(
      std::make_unique<ReadingDayDetailActivity>(renderer, mappedInput, model_.selectedCell()),
      [this](const ActivityResult&) { requestUpdate(); });
}

void ReadingCalendarActivity::loop() {
  if (suppressInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease_ = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  const bool swapped = mappedInput.isNavDirectionSwapped();
  navigator_.onPressAndContinuous({swapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left},
                                  [this] { moveDay(-1); });
  navigator_.onPressAndContinuous({swapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right},
                                  [this] { moveDay(1); });
  navigator_.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveMonth(-1); });
  navigator_.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveMonth(1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelectedDay();
}

void ReadingCalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_STATS_CALENDAR));

  char month[16] = "--/----";
  char selected[16] = "--/--/----";
  if (model_.visibleMonth().isValid()) {
    snprintf(month, sizeof(month), "%02u/%04u", static_cast<unsigned>(model_.visibleMonth().month),
             static_cast<unsigned>(model_.visibleMonth().year));
  }
  if (model_.selectedDate().isValid()) {
    snprintf(selected, sizeof(selected), "%02u/%02u/%04u", static_cast<unsigned>(model_.selectedDate().day),
             static_cast<unsigned>(model_.selectedDate().month), static_cast<unsigned>(model_.selectedDate().year));
  }
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, month, selected);

  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  if (!model_.isAvailable()) {
    const char* message =
        model_.snapshot().clockValid ? tr(STR_STATS_DATED_DATA_UNAVAILABLE) : tr(STR_STATS_CALENDAR_CLOCK_UNAVAILABLE);
    const std::string displayed = renderer.truncatedText(UI_10_FONT_ID, message, safe.width - 32);
    drawCentered(renderer, UI_10_FONT_ID, safe.x + 16, safe.width - 32,
                 contentTop + std::max(0, (contentBottom - contentTop - renderer.getLineHeight(UI_10_FONT_ID)) / 2),
                 displayed.c_str());
  } else {
    const int edge = metrics.contentSidePadding;
    const Rect content{safe.x + edge, contentTop, std::max(1, safe.width - edge * 2),
                       std::max(1, contentBottom - contentTop)};
    const ReadingCalendarMonthSummary summary = model_.monthSummary();
    const bool landscape = safe.width > safe.height;
    if (landscape) {
      const int gap = metrics.verticalSpacing;
      const int leftWidth = std::max(190, content.width * 36 / 100);
      const int legendHeight = renderer.getLineHeight(SMALL_FONT_ID);
      drawSummary(renderer, Rect{content.x, content.y, leftWidth, std::max(1, content.height - legendHeight - gap)},
                  summary, summary.longestReadingStreak);
      ReadingCalendarRenderer::drawLegend(
          renderer, Rect{content.x, content.y + content.height - legendHeight, leftWidth, legendHeight});
      ReadingCalendarRenderer::drawGrid(
          renderer,
          Rect{content.x + leftWidth + gap, content.y, std::max(1, content.width - leftWidth - gap), content.height},
          model_);
    } else {
      const int summaryHeight = std::min(140, std::max(104, content.height / 4));
      const int legendHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int gap = 8;
      drawSummary(renderer, Rect{content.x, content.y, content.width, summaryHeight}, summary,
                  summary.longestReadingStreak);
      const int gridTop = content.y + summaryHeight + gap;
      const Rect grid = ReadingCalendarRenderer::drawGrid(
          renderer,
          Rect{content.x, gridTop, content.width, std::max(1, content.height - summaryHeight - legendHeight - gap * 2)},
          model_);
      ReadingCalendarRenderer::drawLegend(
          renderer, Rect{grid.x, content.y + content.height - legendHeight, grid.width, legendHeight});
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_STATS_DAY_PREVIOUS), tr(STR_STATS_DAY_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
