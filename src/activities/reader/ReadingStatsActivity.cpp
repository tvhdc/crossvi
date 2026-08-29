#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>

#include "BookReadingHistoryActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum class MetricFormat : uint8_t { Duration, Count, Percent, Remaining, DateTime, Pace };

struct SummaryCell {
  StrId label = StrId::STR_STATS_READING_TIME;
  const ReadingStatsMetric* metric = nullptr;
  MetricFormat format = MetricFormat::Count;
};

template <size_t Capacity>
struct SummaryCells {
  std::array<SummaryCell, Capacity> items{};
  size_t count = 0;

  void add(const StrId label, const ReadingStatsMetric& metric, const MetricFormat format,
           const bool includeNoData = false) {
    if (metric.state != ReadingStatsMetricState::Known && metric.state != ReadingStatsMetricState::Estimated &&
        !(includeNoData && metric.state == ReadingStatsMetricState::NoData)) {
      return;
    }
    if (count >= Capacity) return;
    items[count++] = {label, &metric, format};
  }
};

constexpr std::array<StrId, READING_TIME_BUCKET_COUNT> TIME_BUCKET_LABELS = {
    StrId::STR_STATS_MORNING, StrId::STR_STATS_AFTERNOON, StrId::STR_STATS_EVENING, StrId::STR_STATS_NIGHT};
constexpr std::array<StrId, READING_DAY_OF_WEEK_COUNT> DAY_LABELS = {
    StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
    StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};

std::string formatDuration(const uint32_t seconds, const bool estimated) {
  char value[28];
  const char* prefix = estimated ? "~" : "";
  if (seconds == 0) {
    snprintf(value, sizeof(value), "%s0m", prefix);
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "%s<1m", prefix);
  } else {
    const uint32_t minutes = seconds / 60;
    if (minutes < 60) {
      snprintf(value, sizeof(value), "%s%lum", prefix, static_cast<unsigned long>(minutes));
    } else {
      const uint32_t hours = minutes / 60;
      const uint32_t remainder = minutes % 60;
      if (hours < 1000 && remainder > 0) {
        snprintf(value, sizeof(value), "%s%luh %lum", prefix, static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(remainder));
      } else {
        snprintf(value, sizeof(value), "%s%luh", prefix, static_cast<unsigned long>(hours));
      }
    }
  }
  return value;
}

std::string formatPace(const uint32_t seconds) {
  char value[24];
  if (seconds < 60) {
    snprintf(value, sizeof(value), "%lus", static_cast<unsigned long>(seconds));
  } else {
    snprintf(value, sizeof(value), "%lum %lus", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  }
  return value;
}

std::string formatMetric(const ReadingStatsMetric& metric, const MetricFormat format) {
  if (metric.state == ReadingStatsMetricState::NotApplicable) return tr(STR_STATS_NOT_APPLICABLE);
  if (metric.state == ReadingStatsMetricState::NoData) return tr(STR_STATS_NO_DATA);
  if (metric.state == ReadingStatsMetricState::Unavailable) return tr(STR_STATS_UNAVAILABLE);
  const bool estimated = metric.state == ReadingStatsMetricState::Estimated;
  switch (format) {
    case MetricFormat::Duration:
      return formatDuration(metric.value, estimated);
    case MetricFormat::Percent:
      return std::string(estimated ? "~" : "") + std::to_string(metric.value) + "%";
    case MetricFormat::Remaining:
      return metric.state == ReadingStatsMetricState::Known && metric.value == 0
                 ? std::string(tr(STR_STATS_FINISHED))
                 : formatDuration(metric.value, estimated);
    case MetricFormat::DateTime: {
      ReadingStatsDateTime dateTime;
      if (!readingStatsDateTimeFromMinuteIndex(metric.value, dateTime)) return tr(STR_STATS_UNAVAILABLE);
      char value[20];
      snprintf(value, sizeof(value), "%s%02u:%02u %02u/%02u/%02u", estimated ? "~" : "",
               static_cast<unsigned>(dateTime.hour), static_cast<unsigned>(dateTime.minute),
               static_cast<unsigned>(dateTime.date.day), static_cast<unsigned>(dateTime.date.month),
               static_cast<unsigned>(dateTime.date.year % 100u));
      return value;
    }
    case MetricFormat::Pace:
      return formatPace(metric.value);
    case MetricFormat::Count:
    default:
      return std::string(estimated ? "~" : "") + std::to_string(metric.value);
  }
}

void drawCentered(const GfxRenderer& renderer, const int fontId, const int x, const int width, const int y,
                  const char* value, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(fontId, value, style);
  renderer.drawText(fontId, x + std::max(0, (width - textWidth) / 2), y, value, true, style);
}

void drawSummaryCell(const GfxRenderer& renderer, const Rect& rect, const SummaryCell& cell) {
  const int valueLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int textHeight = valueLineHeight + labelLineHeight + 2;
  const int top = rect.y + std::max(2, (rect.height - textHeight) / 2);
  const std::string value = formatMetric(*cell.metric, cell.format);
  const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(cell.label), rect.width - 8);
  drawCentered(renderer, UI_10_FONT_ID, rect.x + 4, rect.width - 8, top, value.c_str(), EpdFontFamily::BOLD);
  drawCentered(renderer, SMALL_FONT_ID, rect.x + 4, rect.width - 8, top + valueLineHeight + 2, label.c_str());
}

void drawSummaryCard(const GfxRenderer& renderer, const Rect& rect, const SummaryCell* cells, const size_t count,
                     const int requestedColumns) {
  if (!cells || count == 0) return;
  const int columns = std::min(requestedColumns, static_cast<int>(count));
  const int rows = (static_cast<int>(count) + columns - 1) / columns;
  const int rowHeight = rect.height / rows;
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  for (int row = 1; row < rows; ++row) {
    renderer.drawLine(rect.x, rect.y + row * rowHeight, rect.x + rect.width - 1, rect.y + row * rowHeight);
  }
  for (int row = 0; row < rows; ++row) {
    const size_t rowStart = static_cast<size_t>(row * columns);
    const int rowCells = std::min(columns, static_cast<int>(count - rowStart));
    const int columnWidth = rect.width / rowCells;
    const int cellY = rect.y + row * rowHeight;
    const int cellHeight = row == rows - 1 ? rect.y + rect.height - cellY : rowHeight;
    for (int column = 1; column < rowCells; ++column) {
      const int dividerX = rect.x + column * columnWidth;
      renderer.drawLine(dividerX, cellY, dividerX, cellY + cellHeight - 1);
    }
  }
  for (size_t index = 0; index < count; ++index) {
    const int row = static_cast<int>(index) / columns;
    const int column = static_cast<int>(index) % columns;
    const size_t rowStart = static_cast<size_t>(row * columns);
    const int rowCells = std::min(columns, static_cast<int>(count - rowStart));
    const int columnWidth = rect.width / rowCells;
    const int cellX = rect.x + column * columnWidth;
    const int cellWidth = column == rowCells - 1 ? rect.x + rect.width - cellX : columnWidth;
    const int cellY = rect.y + row * rowHeight;
    const int cellHeight = row == rows - 1 ? rect.y + rect.height - cellY : rowHeight;
    drawSummaryCell(renderer, Rect{cellX, cellY, cellWidth, cellHeight}, cells[index]);
  }
}

std::string formatChartDuration(const uint32_t seconds) {
  char value[20];
  if (seconds == 0) {
    snprintf(value, sizeof(value), "0m");
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "<1m");
  } else if (seconds < 3600) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(seconds / 60));
  } else if (seconds < 100u * 3600u) {
    snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  } else {
    snprintf(value, sizeof(value), "%luh", static_cast<unsigned long>(seconds / 3600));
  }
  return value;
}

template <size_t N>
void drawChart(GfxRenderer& renderer, const Rect& rect, const StrId title, const ReadingStatsChart<N>& chart,
               const std::array<StrId, N>& labels) {
  if (rect.width <= 2 || rect.height <= 2) return;
  const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int noteHeight = chart.incomplete ? renderer.getLineHeight(SMALL_FONT_ID) + 5 : 0;
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  renderer.drawLine(rect.x, rect.y + titleHeight, rect.x + rect.width - 1, rect.y + titleHeight);
  drawCentered(renderer, UI_10_FONT_ID, rect.x + 4, rect.width - 8, rect.y + 4, I18N.get(title), EpdFontFamily::BOLD);

  if (!chart.available) {
    const int messageY =
        rect.y + titleHeight + std::max(0, (rect.height - titleHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2);
    const std::string message =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_DATED_DATA_UNAVAILABLE), rect.width - 16);
    drawCentered(renderer, SMALL_FONT_ID, rect.x + 8, rect.width - 16, messageY, message.c_str());
    return;
  }

  const int bodyTop = rect.y + titleHeight;
  const int bodyHeight = std::max(1, rect.height - titleHeight - noteHeight);
  const int rowHeight = std::max(1, bodyHeight / static_cast<int>(N));
  int labelWidth = std::accumulate(labels.begin(), labels.end(), 0, [&renderer](const int width, const StrId label) {
    return std::max(width, renderer.getTextWidth(SMALL_FONT_ID, I18N.get(label)));
  });
  labelWidth = std::min(labelWidth + 12, rect.width / 3);
  const int valueWidth = std::max(42, renderer.getTextWidth(SMALL_FONT_ID, "99999h") + 8);
  const int barX = rect.x + 6 + labelWidth;
  const int barWidth = std::max(0, rect.width - labelWidth - valueWidth - 18);
  constexpr uint32_t MINIMUM_CHART_SCALE_SECONDS = 15U * 60U;
  const uint32_t maximum =
      std::max(MINIMUM_CHART_SCALE_SECONDS, *std::max_element(chart.seconds.begin(), chart.seconds.end()));
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  for (size_t index = 0; index < N; ++index) {
    const int rowTop = bodyTop + static_cast<int>(index) * rowHeight;
    const int textY = rowTop + std::max(0, (rowHeight - lineHeight) / 2);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(labels[index]), labelWidth - 4);
    renderer.drawText(SMALL_FONT_ID, rect.x + 6, textY, label.c_str());

    if (barWidth > 2 && rowHeight > 4) {
      const int barHeight = std::max(3, std::min(12, rowHeight - 6));
      const int barY = rowTop + std::max(1, (rowHeight - barHeight) / 2);
      renderer.drawRect(barX, barY, barWidth, barHeight);
      const int fillWidth = scaleReadingStatsBar(chart.seconds[index], maximum, barWidth - 2);
      if (fillWidth > 0) renderer.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2);
    }

    const std::string value = formatChartDuration(chart.seconds[index]);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, value.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - 6 - textWidth, textY, value.c_str());
  }

  if (chart.incomplete) {
    const int noteY = rect.y + rect.height - noteHeight + 2;
    const std::string note = renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_DATED_DATA_PARTIAL), rect.width - 12);
    drawCentered(renderer, SMALL_FONT_ID, rect.x + 6, rect.width - 12, noteY, note.c_str());
  }
}

SummaryCells<5> bookCells(const BookReadingStatsPresentation& model) {
  SummaryCells<5> cells;
  const bool knownIncomplete = model.completed.state == ReadingStatsMetricState::Known && model.completed.value == 0;
  const StrId finishLabel = knownIncomplete || model.finishDate.state == ReadingStatsMetricState::Estimated
                                ? StrId::STR_STATS_EST_FINISH_DATE
                                : StrId::STR_STATS_FINISHED_DATE;
  cells.add(StrId::STR_STATS_READING_TIME, model.readingTime, MetricFormat::Duration, true);
  cells.add(StrId::STR_STATS_PROGRESS, model.progress, MetricFormat::Percent);
  cells.add(StrId::STR_STATS_SESSIONS, model.sessions, MetricFormat::Count, true);
  cells.add(StrId::STR_STATS_STARTED_DATE, model.startDate, MetricFormat::DateTime);
  if (model.finishDate.state == ReadingStatsMetricState::Known ||
      model.finishDate.state == ReadingStatsMetricState::Estimated) {
    cells.add(finishLabel, model.finishDate, MetricFormat::DateTime);
  } else if (model.timeLeft.state != ReadingStatsMetricState::Known || model.timeLeft.value != 0) {
    cells.add(StrId::STR_STATS_TIME_LEFT, model.timeLeft, MetricFormat::Remaining);
  }
  return cells;
}

SummaryCells<6> globalCells(const GlobalReadingStatsPresentation& model) {
  SummaryCells<6> cells;
  cells.add(StrId::STR_STATS_READING_TIME, model.readingTime, MetricFormat::Duration, true);
  cells.add(StrId::STR_STATS_SESSIONS, model.sessions, MetricFormat::Count, true);
  cells.add(StrId::STR_STATS_PAGES_TURNED, model.pagesTurned, MetricFormat::Count, true);
  cells.add(StrId::STR_STATS_COMPLETED_BOOKS, model.completedBooks, MetricFormat::Count, true);
  cells.add(StrId::STR_STATS_STREAK, model.currentStreak, MetricFormat::Count);
  cells.add(StrId::STR_STATS_LONGEST_STREAK, model.longestStreak, MetricFormat::Count);
  return cells;
}

bool isCompletedBook(const ReadingStatsPresentation& presentation) {
  return presentation.book.completed.state == ReadingStatsMetricState::Known && presentation.book.completed.value != 0;
}

std::string compactReadingPattern(const BookReadingStatsPresentation& model) {
  std::string value;
  if (model.preferredTimeBucket.state == ReadingStatsMetricState::Known &&
      model.preferredTimeBucket.value < TIME_BUCKET_LABELS.size()) {
    value = I18N.get(TIME_BUCKET_LABELS[model.preferredTimeBucket.value]);
  }
  if (model.preferredWeekday.state == ReadingStatsMetricState::Known &&
      model.preferredWeekday.value < DAY_LABELS.size()) {
    if (!value.empty()) value += " · ";
    value += I18N.get(DAY_LABELS[model.preferredWeekday.value]);
  }
  return value.empty() ? std::string(tr(STR_STATS_DATED_DATA_UNAVAILABLE)) : value;
}

void renderCompletedBookSummary(const GfxRenderer& renderer, const Rect& content,
                                const BookReadingStatsPresentation& model, const bool landscape) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = metrics.verticalSpacing;
  const int valueHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int heroHeight = valueHeight + labelHeight + 18;
  const Rect hero{content.x, content.y, content.width, heroHeight};
  renderer.drawRect(hero.x, hero.y, hero.width, hero.height);
  const std::string total = formatMetric(model.readingTime, MetricFormat::Duration);
  drawCentered(renderer, UI_12_FONT_ID, hero.x + 6, hero.width - 12, hero.y + 7, total.c_str(), EpdFontFamily::BOLD);
  drawCentered(renderer, SMALL_FONT_ID, hero.x + 6, hero.width - 12, hero.y + 9 + valueHeight,
               tr(STR_STATS_READING_TIME));

  SummaryCells<2> dates;
  dates.add(StrId::STR_STATS_STARTED_DATE, model.startDate, MetricFormat::DateTime, true);
  dates.add(StrId::STR_STATS_FINISHED_DATE, model.finishDate, MetricFormat::DateTime, true);
  const int dateTop = hero.y + hero.height + gap;
  const int dateHeight = valueHeight + labelHeight + 12;
  drawSummaryCard(renderer, Rect{content.x, dateTop, content.width, dateHeight}, dates.items.data(), dates.count, 2);

  SummaryCells<4> details;
  details.add(StrId::STR_STATS_SESSIONS, model.sessions, MetricFormat::Count, true);
  details.add(StrId::STR_STATS_PAGES_TURNED, model.pagesTurned, MetricFormat::Count, true);
  details.add(StrId::STR_STATS_TIME_PER_PAGE, model.averagePage, MetricFormat::Pace);
  details.add(StrId::STR_STATS_DAYS_TO_FINISH, model.completionDays, MetricFormat::Count);
  const int columns = landscape ? 4 : 2;
  const int rows = details.count == 0 ? 0 : (static_cast<int>(details.count) + columns - 1) / columns;
  const int detailsTop = dateTop + dateHeight + gap;
  const int detailsHeight = rows * (valueHeight + labelHeight + 10);
  if (detailsHeight > 0) {
    drawSummaryCard(renderer, Rect{content.x, detailsTop, content.width, detailsHeight}, details.items.data(),
                    details.count, columns);
  }

  const int patternTop = detailsTop + detailsHeight + (detailsHeight > 0 ? gap : 0);
  const int patternHeight = content.y + content.height - patternTop;
  if (patternHeight < labelHeight * 2 + 10) return;
  renderer.drawRect(content.x, patternTop, content.width, patternHeight);
  drawCentered(renderer, SMALL_FONT_ID, content.x + 6, content.width - 12, patternTop + 5,
               tr(STR_STATS_READING_PATTERN), EpdFontFamily::BOLD);
  const std::string pattern = compactReadingPattern(model);
  drawCentered(renderer, UI_10_FONT_ID, content.x + 6, content.width - 12,
               patternTop + std::max(labelHeight + 7, (patternHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2),
               pattern.c_str());
}
}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string bookTitle, ReadingStatsPresentation presentation,
                                           const Page initialPage, const bool allowBookDateEdit,
                                           const bool allowDeviceBackup, std::string bookPath)
    : Activity("ReadingStats", renderer, mappedInput),
      bookTitle(std::move(bookTitle)),
      bookPath(std::move(bookPath)),
      presentation(std::move(presentation)),
      page(initialPage),
      allowBookDateEdit(allowBookDateEdit),
      allowDeviceBackup(allowDeviceBackup) {}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  if (suppressInitialConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease = false;
    }
    return;
  }

  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (page == Page::Book && allowBookDateEdit) {
    setResult(ReadingStatsActionResult{ReadingStatsActionResult::Action::EditBookDates});
    finish();
    return;
  }
  if (page == Page::Book && isCompletedBook(presentation) && !bookPath.empty()) {
    startActivityForResult(std::make_unique<BookReadingHistoryActivity>(renderer, mappedInput, bookPath, bookTitle),
                           [](const ActivityResult&) {});
    return;
  }
  if (page == Page::Device) {
    if (!allowDeviceBackup) return;
    static constexpr std::array<StrId, 3> options = {StrId::STR_STATS_BACKUP, StrId::STR_STATS_RESTORE,
                                                     StrId::STR_VCODEX_IMPORT_ACTION};
    optionPopup.show(
        StrId::STR_STATS_MANAGE, options.data(), static_cast<int>(options.size()), 0, [this](const int selected) {
          static constexpr std::array<ReadingStatsActionResult::Action, 3> actions = {
              ReadingStatsActionResult::Action::BackupDeviceStats, ReadingStatsActionResult::Action::RestoreDeviceStats,
              ReadingStatsActionResult::Action::ImportVCodexStats};
          const auto action = actions[static_cast<size_t>(selected)];
          setResult(ReadingStatsActionResult{action});
          finish();
        });
    requestUpdate();
    return;
  }
  finish();
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safeArea.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  // getScreenSafeArea(..., true, ...) already removes the physical front-button
  // strip in every orientation. Keep an additional content gap inside that
  // safe area so charts can never be painted under the button hints.
  const int contentBottom = safeArea.y + safeArea.height - metrics.verticalSpacing;
  const int cardX = safeArea.x + metrics.contentSidePadding;
  const int cardWidth = std::max(1, safeArea.width - metrics.contentSidePadding * 2);
  const bool landscape = safeArea.width > safeArea.height;

  const GlobalReadingStatsPresentation* globalModel = nullptr;
  StrId pageTitle = StrId::STR_STATS_THIS_BOOK;
  std::string pageTitleText;
  if (page == Page::Device) {
    pageTitle = StrId::STR_STATS_OVERVIEW;
    globalModel = &presentation.device;
  }
  pageTitleText = I18N.get(pageTitle);

  GUI.drawHeader(renderer, Rect{safeArea.x, headerTop, safeArea.width, metrics.headerHeight}, tr(STR_READING_STATS),
                 page == Page::Book ? bookTitle.c_str() : nullptr);
  GUI.drawSubHeader(renderer, Rect{safeArea.x, subHeaderTop, safeArea.width, metrics.tabBarHeight},
                    pageTitleText.c_str());

  const bool completedBook = page == Page::Book && isCompletedBook(presentation);
  if (completedBook) {
    const Rect content{cardX, contentTop, cardWidth, std::max(1, contentBottom - contentTop)};
    renderCompletedBookSummary(renderer, content, presentation.book, landscape);
    const StrId confirmLabel = allowBookDateEdit   ? StrId::STR_STATS_EDIT_DATES
                               : !bookPath.empty() ? StrId::STR_STATS_READING_HISTORY
                                                   : StrId::STR_DONE;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), I18N.get(confirmLabel), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const SummaryCells<5> bookSummary = bookCells(presentation.book);
  const SummaryCells<6> globalSummary = globalModel ? globalCells(*globalModel) : SummaryCells<6>{};
  const SummaryCell* summaryCells = page == Page::Book ? bookSummary.items.data() : globalSummary.items.data();
  const size_t summaryCount = page == Page::Book ? bookSummary.count : globalSummary.count;
  const int summaryColumns = landscape ? std::min<int>(4, summaryCount) : std::min<int>(2, summaryCount);
  const int summaryRows =
      summaryColumns > 0 ? (static_cast<int>(summaryCount) + summaryColumns - 1) / summaryColumns : 0;
  const int summaryRowHeight = renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int summaryHeight = summaryRows * summaryRowHeight;
  const Rect summaryRect{cardX, contentTop, cardWidth, std::max(1, summaryHeight)};
  if (summaryCount > 0) drawSummaryCard(renderer, summaryRect, summaryCells, summaryCount, summaryColumns);

  int chartsTop = contentTop + summaryHeight + (summaryHeight > 0 ? metrics.verticalSpacing : 0);
  const int chartHeight = std::max(1, contentBottom - chartsTop);
  const auto& timeChart = page == Page::Book ? presentation.book.timeOfDay : globalModel->timeOfDay;
  const auto& dayChart = page == Page::Book ? presentation.book.dayOfWeek : globalModel->dayOfWeek;
  if (landscape) {
    const int gap = metrics.verticalSpacing;
    const int firstWidth = std::max(1, (cardWidth - gap) / 2);
    drawChart(renderer, Rect{cardX, chartsTop, firstWidth, chartHeight}, StrId::STR_STATS_TIME_OF_DAY, timeChart,
              TIME_BUCKET_LABELS);
    drawChart(renderer,
              Rect{cardX + firstWidth + gap, chartsTop, std::max(1, cardWidth - firstWidth - gap), chartHeight},
              StrId::STR_STATS_DAY_OF_WEEK, dayChart, DAY_LABELS);
  } else {
    const int gap = metrics.verticalSpacing;
    const int firstHeight = std::max(1, (chartHeight - gap) * 5 / 12);
    drawChart(renderer, Rect{cardX, chartsTop, cardWidth, firstHeight}, StrId::STR_STATS_TIME_OF_DAY, timeChart,
              TIME_BUCKET_LABELS);
    drawChart(renderer,
              Rect{cardX, chartsTop + firstHeight + gap, cardWidth, std::max(1, chartHeight - firstHeight - gap)},
              StrId::STR_STATS_DAY_OF_WEEK, dayChart, DAY_LABELS);
  }

  const StrId confirmLabel = page == Page::Device                      ? StrId::STR_STATS_MANAGE
                             : page == Page::Book && allowBookDateEdit ? StrId::STR_STATS_EDIT_DATES
                                                                       : StrId::STR_DONE;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), I18N.get(confirmLabel), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (optionPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
