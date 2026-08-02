#include "ReadingCalendarActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>

#include "MappedInputManager.h"
#include "ReadingCalendarLayout.h"
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

std::string formatDuration(const uint32_t seconds, const bool minimum = false) {
  char duration[28];
  if (seconds == 0) {
    snprintf(duration, sizeof(duration), "0m");
  } else if (seconds < 60) {
    snprintf(duration, sizeof(duration), "<1m");
  } else if (seconds < 3600) {
    snprintf(duration, sizeof(duration), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(duration, sizeof(duration), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  }
  return minimum ? std::string(tr(STR_STATS_MINIMUM_PREFIX)) + " " + duration : duration;
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
    const std::string duration = formatDuration(summary.bestDaySeconds);
    snprintf(best, sizeof(best), "%s · %u", duration.c_str(), static_cast<unsigned>(summary.bestDayOfMonth));
  }
  drawMetric(renderer, Rect{rect.x, rect.y, cellWidth, cellHeight},
             formatDuration(summary.totalSeconds, summary.totalIsMinimum), StrId::STR_STATS_MONTH_TOTAL);
  drawMetric(renderer, Rect{rect.x + cellWidth + gap, rect.y, rect.width - cellWidth - gap, cellHeight}, count,
             StrId::STR_STATS_MONTH_DAYS);
  drawMetric(renderer, Rect{rect.x, rect.y + cellHeight + gap, cellWidth, rect.height - cellHeight - gap}, best,
             StrId::STR_STATS_BEST_DAY);
  drawMetric(renderer,
             Rect{rect.x + cellWidth + gap, rect.y + cellHeight + gap, rect.width - cellWidth - gap,
                  rect.height - cellHeight - gap},
             streak, StrId::STR_STATS_LONGEST_STREAK);
}

void fillHeatmapCell(const GfxRenderer& renderer, const Rect& rect, const ReadingHeatmapLevel level) {
  if (rect.width <= 0 || rect.height <= 0) return;
  switch (level) {
    case ReadingHeatmapLevel::None:
      return;
    case ReadingHeatmapLevel::Minutes15:
      for (int y = rect.y; y < rect.y + rect.height; y += 4) {
        const int offset = (y / 4 & 1) * 2;
        for (int x = rect.x + offset; x < rect.x + rect.width; x += 4) renderer.drawPixel(x, y);
      }
      return;
    case ReadingHeatmapLevel::Minutes30:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      return;
    case ReadingHeatmapLevel::Minutes60:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::DarkGray);
      return;
    case ReadingHeatmapLevel::Minutes120:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height);
      for (int y = rect.y; y < rect.y + rect.height; y += 2) {
        for (int x = rect.x + (y & 2) / 2; x < rect.x + rect.width; x += 2) renderer.drawPixel(x, y, false);
      }
      return;
    case ReadingHeatmapLevel::Minutes240:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height);
      return;
  }
}

bool usesWhiteDayNumber(const ReadingHeatmapLevel level) {
  return level == ReadingHeatmapLevel::Minutes120 || level == ReadingHeatmapLevel::Minutes240;
}

Rect drawCalendarGrid(const GfxRenderer& renderer, const Rect& rect, const ReadingCalendarModel& model) {
  const ReadingCalendarGridLayout layout = ReadingCalendarGridLayout::calculate(rect, 0);
  if (layout.cellSize <= 0) return Rect{};
  for (size_t index = 0; index < ReadingCalendarGridLayout::CELL_COUNT; ++index) {
    const ReadingCalendarCell cell = model.cellAt(index);
    if (!cell.date.isValid()) continue;
    const Rect box = layout.cell(index);
    const ReadingHeatmapLevel level =
        cell.exactDuration ? readingHeatmapLevel(cell.readingSeconds) : ReadingHeatmapLevel::None;
    const Rect fill{box.x + 1, box.y + 1, std::max(0, box.width - 2), std::max(0, box.height - 2)};
    fillHeatmapCell(renderer, fill, level);
    renderer.drawRect(box.x, box.y, box.width, box.height, cell.selected ? 2 : 1, true);

    char day[4];
    snprintf(day, sizeof(day), "%u", static_cast<unsigned>(cell.date.day));
    const bool blackText = !usesWhiteDayNumber(level);
    renderer.drawText(SMALL_FONT_ID, box.x + 4, box.y + 3, day, blackText, EpdFontFamily::BOLD);

    if (cell.today) {
      renderer.fillRect(box.x + box.width - 8, box.y + 4, 4, 4, blackText);
    } else if (cell.legacyDuration) {
      renderer.drawLine(box.x + 5, box.y + box.height - 5, box.x + box.width - 6, box.y + box.height - 5, blackText);
    } else if (cell.exactDuration && cell.readingSeconds > 0 && level == ReadingHeatmapLevel::None) {
      renderer.fillRect(box.x + box.width / 2 - 2, box.y + box.height - 7, 4, 3, blackText);
    }
  }
  return layout.grid;
}

void drawLegend(const GfxRenderer& renderer, const Rect& rect) {
  constexpr std::array<StrId, 5> labels = {StrId::STR_STATS_INTENSITY_15, StrId::STR_STATS_INTENSITY_30,
                                           StrId::STR_STATS_INTENSITY_60, StrId::STR_STATS_INTENSITY_120,
                                           StrId::STR_STATS_INTENSITY_240};
  constexpr std::array<ReadingHeatmapLevel, 5> levels = {
      ReadingHeatmapLevel::Minutes15, ReadingHeatmapLevel::Minutes30, ReadingHeatmapLevel::Minutes60,
      ReadingHeatmapLevel::Minutes120, ReadingHeatmapLevel::Minutes240};
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int markerSize = std::max(7, std::min(12, lineHeight - 2));
  for (size_t index = 0; index < labels.size(); ++index) {
    const char* label = I18N.get(labels[index]);
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int groupWidth = markerSize + 3 + labelWidth;
    const int anchor =
        rect.x + static_cast<int>(index) * std::max(0, rect.width - 1) / static_cast<int>(labels.size() - 1);
    const int groupX = index == 0                   ? rect.x
                       : index + 1 == labels.size() ? rect.x + rect.width - groupWidth
                                                    : anchor - groupWidth / 2;
    const Rect marker{groupX, rect.y + std::max(0, (lineHeight - markerSize) / 2), markerSize, markerSize};
    fillHeatmapCell(renderer, Rect{marker.x + 1, marker.y + 1, marker.width - 2, marker.height - 2}, levels[index]);
    renderer.drawRect(marker.x, marker.y, marker.width, marker.height);
    renderer.drawText(SMALL_FONT_ID, marker.x + marker.width + 3, rect.y, label);
  }
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
      std::make_unique<ReadingDayDetailActivity>(renderer, mappedInput, model_.selectedCell(), model_.snapshot()),
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
      drawLegend(renderer, Rect{content.x, content.y + content.height - legendHeight, leftWidth, legendHeight});
      drawCalendarGrid(
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
      const Rect grid = drawCalendarGrid(
          renderer,
          Rect{content.x, gridTop, content.width, std::max(1, content.height - summaryHeight - legendHeight - gap * 2)},
          model_);
      drawLegend(renderer, Rect{grid.x, content.y + content.height - legendHeight, grid.width, legendHeight});
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_STATS_DAY_PREVIOUS), tr(STR_STATS_DAY_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
