#include <gtest/gtest.h>

#include <array>

#include "ReadingCalendarLayout.h"
#include "ReadingCalendarModel.h"

namespace {
void setHistoryBit(ReadingCalendarSnapshot& snapshot, const size_t index) {
  snapshot.historyBits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
  ++snapshot.readingDays;
}

void setHistoryDay(ReadingCalendarSnapshot& snapshot, const ReadingStatsDate& date) {
  const uint32_t day = readingStatsDayIndex(date);
  ASSERT_LE(day, snapshot.anchorDay);
  setHistoryBit(snapshot, snapshot.anchorDay - day);
}

ReadingCalendarCell findDay(const ReadingCalendarModel& model, const uint8_t day) {
  for (size_t index = 0; index < 42; ++index) {
    const ReadingCalendarCell cell = model.cellAt(index);
    if (cell.inMonth && cell.date.day == day) return cell;
  }
  return {};
}

bool contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.width >= 0 && inner.height >= 0 &&
         inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}
}  // namespace

TEST(ReadingCalendarModel, MapsTheFiveHeatmapThresholdsExactly) {
  EXPECT_EQ(readingHeatmapLevel(0), ReadingHeatmapLevel::None);
  EXPECT_EQ(readingHeatmapLevel(15u * 60u - 1), ReadingHeatmapLevel::None);
  EXPECT_EQ(readingHeatmapLevel(15u * 60u), ReadingHeatmapLevel::Minutes15);
  EXPECT_EQ(readingHeatmapLevel(30u * 60u - 1), ReadingHeatmapLevel::Minutes15);
  EXPECT_EQ(readingHeatmapLevel(30u * 60u), ReadingHeatmapLevel::Minutes30);
  EXPECT_EQ(readingHeatmapLevel(60u * 60u - 1), ReadingHeatmapLevel::Minutes30);
  EXPECT_EQ(readingHeatmapLevel(60u * 60u), ReadingHeatmapLevel::Minutes60);
  EXPECT_EQ(readingHeatmapLevel(120u * 60u - 1), ReadingHeatmapLevel::Minutes60);
  EXPECT_EQ(readingHeatmapLevel(120u * 60u), ReadingHeatmapLevel::Minutes120);
  EXPECT_EQ(readingHeatmapLevel(240u * 60u - 1), ReadingHeatmapLevel::Minutes120);
  EXPECT_EQ(readingHeatmapLevel(240u * 60u), ReadingHeatmapLevel::Minutes240);
}

TEST(ReadingCalendarLayout, KeepsAllFortyTwoCellsSquareOnX3AndX4Layouts) {
  constexpr int weekdayHeight = 22;
  const std::array<Rect, 4> bounds = {
      Rect{50, 286, 428, 410},   // X3 portrait content width
      Rect{50, 286, 380, 418},   // X4 portrait content width
      Rect{280, 145, 402, 367},  // X3 landscape calendar column
      Rect{252, 145, 358, 375},  // X4 landscape calendar column
  };
  for (const Rect& bound : bounds) {
    const ReadingCalendarGridLayout layout = ReadingCalendarGridLayout::calculate(bound, weekdayHeight);
    ASSERT_GT(layout.cellSize, 0);
    EXPECT_EQ(layout.grid.width, layout.cellSize * ReadingCalendarGridLayout::COLUMNS +
                                     ReadingCalendarGridLayout::CELL_GAP * (ReadingCalendarGridLayout::COLUMNS - 1));
    EXPECT_EQ(layout.grid.height, layout.cellSize * ReadingCalendarGridLayout::ROWS +
                                      ReadingCalendarGridLayout::CELL_GAP * (ReadingCalendarGridLayout::ROWS - 1));
    EXPECT_TRUE(contains(bound, layout.weekdays));
    EXPECT_TRUE(contains(bound, layout.grid));
    for (size_t index = 0; index < ReadingCalendarGridLayout::CELL_COUNT; ++index) {
      const Rect cell = layout.cell(index);
      EXPECT_EQ(cell.width, cell.height);
      EXPECT_EQ(cell.width, layout.cellSize);
      EXPECT_TRUE(contains(layout.grid, cell));
    }
    EXPECT_EQ(layout.cell(0).x, layout.grid.x);
    EXPECT_EQ(layout.cell(0).y, layout.grid.y);
    EXPECT_EQ(layout.grid.y, bound.y + weekdayHeight);
    EXPECT_EQ(layout.cell(1).x - (layout.cell(0).x + layout.cell(0).width), ReadingCalendarGridLayout::CELL_GAP);
    EXPECT_EQ(layout.cell(7).y - (layout.cell(0).y + layout.cell(0).height), ReadingCalendarGridLayout::CELL_GAP);
    const Rect last = layout.cell(ReadingCalendarGridLayout::CELL_COUNT - 1);
    EXPECT_EQ(last.x + last.width, layout.grid.x + layout.grid.width);
    EXPECT_EQ(last.y + last.height, layout.grid.y + layout.grid.height);
    EXPECT_EQ(layout.cell(ReadingCalendarGridLayout::CELL_COUNT).width, 0);
  }
}

TEST(ReadingCalendarLayout, CompactHeatmapStartsAtTheTopAndStaysHorizontallyCentered) {
  const std::array<Rect, 2> bounds = {
      Rect{20, 293, 488, 412},  // X3 portrait after the one-line legend
      Rect{20, 293, 440, 420},  // X4 portrait after the one-line legend
  };
  for (const Rect& bound : bounds) {
    const ReadingCalendarGridLayout layout = ReadingCalendarGridLayout::calculate(bound, 0);
    ASSERT_GT(layout.cellSize, 0);
    EXPECT_EQ(layout.weekdays.height, 0);
    EXPECT_EQ(layout.grid.y, bound.y);
    const int leftGap = layout.grid.x - bound.x;
    const int rightGap = bound.x + bound.width - (layout.grid.x + layout.grid.width);
    EXPECT_LE(leftGap, rightGap + 1);
    EXPECT_LE(rightGap, leftGap + 1);
    EXPECT_EQ(layout.grid.width, layout.cellSize * ReadingCalendarGridLayout::COLUMNS +
                                     ReadingCalendarGridLayout::CELL_GAP * (ReadingCalendarGridLayout::COLUMNS - 1));
    EXPECT_EQ(layout.grid.height, layout.cellSize * ReadingCalendarGridLayout::ROWS +
                                      ReadingCalendarGridLayout::CELL_GAP * (ReadingCalendarGridLayout::ROWS - 1));
    EXPECT_TRUE(contains(bound, layout.grid));
  }
}

TEST(ReadingCalendarModel, RequiresTrustedHistoryAndAValidClock) {
  ReadingCalendarSnapshot snapshot;
  snapshot.historyAvailable = true;
  ReadingCalendarModel noClock(snapshot);
  EXPECT_FALSE(noClock.isAvailable());
  EXPECT_FALSE(noClock.canMovePrevious());
  EXPECT_FALSE(noClock.canMoveNext());

  snapshot.today = {2026, 7, 24};
  snapshot.clockValid = true;
  snapshot.historyAvailable = false;
  ReadingCalendarModel noHistory(snapshot);
  EXPECT_FALSE(noHistory.isAvailable());
}

TEST(ReadingCalendarModel, MapsLeapMonthAndBinaryReadDays) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2024, 2, 29};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);   // 29 February
  setHistoryBit(snapshot, 28);  // 1 February

  ReadingCalendarModel model(snapshot);
  const ReadingCalendarCell first = findDay(model, 1);
  const ReadingCalendarCell last = findDay(model, 29);
  ASSERT_TRUE(first.inMonth);
  ASSERT_TRUE(last.inMonth);
  EXPECT_EQ(readingStatsDayOfWeekIndex(first.date), 3u);  // Thursday, Monday = 0
  EXPECT_TRUE(first.tracked);
  EXPECT_TRUE(first.read);
  EXPECT_TRUE(last.today);
  EXPECT_TRUE(last.read);
  EXPECT_FALSE(findDay(model, 30).inMonth);
}

TEST(ReadingCalendarModel, UsesAllSixRowsWhenTheMonthNeedsThem) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2021, 5, 31};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);

  ReadingCalendarModel model(snapshot);
  const ReadingCalendarCell last = model.cellAt(35);
  ASSERT_TRUE(last.inMonth);
  EXPECT_EQ(last.date.day, 31u);
  EXPECT_TRUE(last.read);
}

TEST(ReadingCalendarModel, DistinguishesTheOldestTrackedDayFromOlderUnknownDays) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);
  setHistoryBit(snapshot, READING_HISTORY_DAYS - 1);

  ReadingStatsDate oldest;
  ASSERT_TRUE(readingStatsDateFromDayIndex(snapshot.anchorDay - (READING_HISTORY_DAYS - 1), oldest));
  ReadingCalendarModel model(snapshot);
  int moves = 0;
  while (model.movePrevious()) ++moves;
  EXPECT_GT(moves, 0);
  EXPECT_EQ(model.visibleMonth().year, oldest.year);
  EXPECT_EQ(model.visibleMonth().month, oldest.month);
  EXPECT_FALSE(model.canMovePrevious());

  const ReadingCalendarCell oldestCell = findDay(model, oldest.day);
  ASSERT_TRUE(oldestCell.inMonth);
  EXPECT_TRUE(oldestCell.tracked);
  EXPECT_TRUE(oldestCell.read);
  if (oldest.day > 1) {
    const ReadingCalendarCell before = findDay(model, static_cast<uint8_t>(oldest.day - 1));
    ASSERT_TRUE(before.inMonth);
    EXPECT_FALSE(before.tracked);
    EXPECT_FALSE(before.read);
  }
}

TEST(ReadingCalendarModel, EmptyHistoryShowsOnlyTheCurrentMonthAsKnown) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;

  ReadingCalendarModel model(snapshot);
  EXPECT_FALSE(model.canMovePrevious());
  EXPECT_FALSE(model.canMoveNext());
  const ReadingCalendarCell today = findDay(model, 24);
  const ReadingCalendarCell future = findDay(model, 25);
  ASSERT_TRUE(today.inMonth);
  ASSERT_TRUE(future.inMonth);
  EXPECT_TRUE(today.tracked);
  EXPECT_FALSE(today.read);
  EXPECT_FALSE(future.tracked);
}

TEST(ReadingCalendarModel, IncludesAdjacentMonthDatesInAllFortyTwoCells) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  ReadingCalendarModel model(snapshot);

  const ReadingCalendarCell first = model.cellAt(0);
  const ReadingCalendarCell last = model.cellAt(41);
  ASSERT_TRUE(first.date.isValid());
  ASSERT_TRUE(last.date.isValid());
  EXPECT_FALSE(first.inMonth);
  EXPECT_EQ(first.date.month, 6u);
  EXPECT_FALSE(last.inMonth);
  EXPECT_EQ(last.date.month, 8u);
}

TEST(ReadingCalendarModel, MovesSelectedDayAcrossMonthAndYearWithoutLeavingTrackedRange) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 1, 2};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryDay(snapshot, {2025, 12, 31});
  ReadingCalendarModel model(snapshot);

  ASSERT_TRUE(model.moveSelectedDay(-2));
  EXPECT_EQ(model.selectedDate().year, 2025u);
  EXPECT_EQ(model.selectedDate().month, 12u);
  EXPECT_EQ(model.selectedDate().day, 31u);
  EXPECT_EQ(model.visibleMonth().month, 12u);
  ASSERT_TRUE(model.moveSelectedDay(2));
  EXPECT_EQ(model.selectedDate().year, 2026u);
  EXPECT_EQ(model.selectedDate().month, 1u);
  EXPECT_FALSE(model.moveSelectedDay(1));
}

TEST(ReadingCalendarModel, MonthNavigationClampsSelectedDayForShorterMonths) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2024, 3, 31};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryDay(snapshot, {2024, 2, 1});
  ReadingCalendarModel model(snapshot);

  ASSERT_TRUE(model.movePrevious());
  EXPECT_EQ(model.selectedDate().year, 2024u);
  EXPECT_EQ(model.selectedDate().month, 2u);
  EXPECT_EQ(model.selectedDate().day, 29u);
}

TEST(ReadingCalendarModel, ExactAndLegacyDurationsProduceHonestMonthMetrics) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryDay(snapshot, {2026, 7, 20});
  setHistoryDay(snapshot, {2026, 7, 21});
  setHistoryDay(snapshot, {2026, 7, 22});
  setHistoryDay(snapshot, {2026, 7, 23});

  DailyReadingHistory history;
  history.seedExactDay(readingStatsDayIndex({2026, 7, 20}), 15u * 60u);
  history.seedExactDay(readingStatsDayIndex({2026, 7, 21}), 30u * 60u);
  history.seedExactDay(readingStatsDayIndex({2026, 7, 22}), 60u * 60u);
  ReadingCalendarModel model(snapshot);
  model.setDailyHistory(&history);

  const ReadingCalendarMonthSummary summary = model.monthSummary();
  EXPECT_EQ(summary.totalSeconds, 105u * 60u);
  EXPECT_TRUE(summary.totalIsMinimum);
  EXPECT_EQ(summary.readingDays, 4u);
  EXPECT_FALSE(summary.bestDayKnown);
  EXPECT_EQ(summary.longestReadingStreak, 4u);

  const ReadingCalendarCell legacy = findDay(model, 23);
  EXPECT_TRUE(legacy.read);
  EXPECT_TRUE(legacy.legacyDuration);
  EXPECT_FALSE(legacy.exactDuration);
  const ReadingCalendarCell exact = findDay(model, 22);
  EXPECT_TRUE(exact.exactDuration);
  EXPECT_EQ(exact.readingSeconds, 60u * 60u);
}

TEST(ReadingCalendarModel, BestDayShowsCalendarDayAndKeepsEarliestTie) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryDay(snapshot, {2026, 7, 20});
  setHistoryDay(snapshot, {2026, 7, 21});
  setHistoryDay(snapshot, {2026, 7, 22});

  DailyReadingHistory history;
  history.seedExactDay(readingStatsDayIndex({2026, 7, 20}), 15u * 60u);
  history.seedExactDay(readingStatsDayIndex({2026, 7, 21}), 2u * 60u * 60u);
  history.seedExactDay(readingStatsDayIndex({2026, 7, 22}), 2u * 60u * 60u);

  ReadingCalendarModel model(snapshot);
  model.setDailyHistory(&history);

  const ReadingCalendarMonthSummary summary = model.monthSummary();
  EXPECT_TRUE(summary.bestDayKnown);
  EXPECT_EQ(summary.bestDaySeconds, 2u * 60u * 60u);
  EXPECT_EQ(summary.bestDayOfMonth, 21u);
}
