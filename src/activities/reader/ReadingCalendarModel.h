#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DailyReadingHistory.h"
#include "ReadingStatsUtils.h"

struct ReadingCalendarSnapshot {
  ReadingStatsDate today;
  uint32_t anchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> historyBits{};
  uint16_t readingDays = 0;
  uint16_t currentStreak = 0;
  uint32_t latestReadingDay = 0;
  uint32_t latestDayReadingSeconds = 0;
  uint32_t latestDaySessions = 0;
  bool clockValid = false;
  bool historyAvailable = false;
  bool hasLatestDayReadingSeconds = false;
};

struct ReadingCalendarCell {
  ReadingStatsDate date;
  bool inMonth = false;
  bool tracked = false;
  bool read = false;
  bool today = false;
  bool selected = false;
  bool future = false;
  bool exactDuration = false;
  bool legacyDuration = false;
  uint32_t readingSeconds = 0;
};

struct ReadingCalendarMonthSummary {
  uint32_t totalSeconds = 0;
  uint32_t bestDaySeconds = 0;
  uint8_t readingDays = 0;
  // Day of month with the highest exact reading duration. If multiple days tie,
  // the earliest day in the visible month is retained.
  uint8_t bestDayOfMonth = 0;
  uint8_t longestReadingStreak = 0;
  bool totalIsMinimum = false;
  bool bestDayKnown = false;
};

enum class ReadingHeatmapLevel : uint8_t {
  None,
  Minutes15,
  Minutes30,
  Minutes60,
  Minutes120,
  Minutes240,
};

ReadingHeatmapLevel readingHeatmapLevel(uint32_t seconds);

class ReadingCalendarModel {
 public:
  explicit ReadingCalendarModel(ReadingCalendarSnapshot snapshot);

  bool isAvailable() const { return snapshot_.clockValid && snapshot_.historyAvailable; }
  const ReadingStatsDate& visibleMonth() const { return visibleMonth_; }
  const ReadingStatsDate& selectedDate() const { return selectedDate_; }
  const ReadingCalendarSnapshot& snapshot() const { return snapshot_; }
  ReadingCalendarCell cellAt(size_t index) const;
  ReadingCalendarCell selectedCell() const;
  ReadingCalendarMonthSummary monthSummary() const;

  void setDailyHistory(const DailyReadingHistory* history) { dailyHistory_ = history; }

  bool canMovePrevious() const;
  bool canMoveNext() const;
  bool movePrevious();
  bool moveNext();
  bool moveSelectedDay(int delta);

 private:
  uint32_t earliestTrackedDay() const;
  bool isReadDay(uint32_t dayIndex) const;
  bool moveMonth(int delta);

  ReadingCalendarSnapshot snapshot_;
  ReadingStatsDate visibleMonth_;
  ReadingStatsDate selectedDate_;
  const DailyReadingHistory* dailyHistory_ = nullptr;
};
