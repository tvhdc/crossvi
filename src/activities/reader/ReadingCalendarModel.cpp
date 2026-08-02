#include "ReadingCalendarModel.h"

#include <algorithm>
#include <utility>

namespace {
ReadingStatsDate firstOfMonth(const ReadingStatsDate& date) {
  return date.isValid() ? ReadingStatsDate{date.year, date.month, 1} : ReadingStatsDate{};
}

bool bitSet(const std::array<uint8_t, READING_HISTORY_BYTES>& bits, const uint32_t index) {
  return index < READING_HISTORY_DAYS && (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}
}  // namespace

ReadingHeatmapLevel readingHeatmapLevel(const uint32_t seconds) {
  if (seconds >= 240u * 60u) return ReadingHeatmapLevel::Minutes240;
  if (seconds >= 120u * 60u) return ReadingHeatmapLevel::Minutes120;
  if (seconds >= 60u * 60u) return ReadingHeatmapLevel::Minutes60;
  if (seconds >= 30u * 60u) return ReadingHeatmapLevel::Minutes30;
  if (seconds >= 15u * 60u) return ReadingHeatmapLevel::Minutes15;
  return ReadingHeatmapLevel::None;
}

ReadingCalendarModel::ReadingCalendarModel(ReadingCalendarSnapshot snapshot) : snapshot_(std::move(snapshot)) {
  visibleMonth_ = firstOfMonth(snapshot_.today);
  selectedDate_ = snapshot_.today;
}

uint32_t ReadingCalendarModel::earliestTrackedDay() const {
  if (snapshot_.readingDays == 0) return readingStatsDayIndex(firstOfMonth(snapshot_.today));
  const uint32_t earliest = snapshot_.anchorDay >= READING_HISTORY_DAYS - 1
                                ? snapshot_.anchorDay - static_cast<uint32_t>(READING_HISTORY_DAYS - 1)
                                : 0;
  return std::min(earliest, readingStatsDayIndex(snapshot_.today));
}

bool ReadingCalendarModel::isReadDay(const uint32_t dayIndex) const {
  if (dayIndex > snapshot_.anchorDay) return false;
  const uint32_t delta = snapshot_.anchorDay - dayIndex;
  return delta < READING_HISTORY_DAYS && bitSet(snapshot_.historyBits, delta);
}

ReadingCalendarCell ReadingCalendarModel::cellAt(const size_t index) const {
  ReadingCalendarCell cell;
  if (!isAvailable() || !visibleMonth_.isValid() || index >= 42) return cell;
  const uint8_t firstColumn = readingStatsDayOfWeekIndex(visibleMonth_);
  cell.date = visibleMonth_;
  addDaysToReadingStatsDate(cell.date, static_cast<int>(index) - firstColumn);
  if (!cell.date.isValid()) return {};
  const uint32_t todayIndex = readingStatsDayIndex(snapshot_.today);
  const uint32_t earliest = earliestTrackedDay();
  const uint32_t dayIndex = readingStatsDayIndex(cell.date);
  cell.inMonth = cell.date.year == visibleMonth_.year && cell.date.month == visibleMonth_.month;
  cell.today = dayIndex == todayIndex;
  cell.selected = compareReadingStatsDate(cell.date, selectedDate_) == 0;
  cell.future = dayIndex > todayIndex;
  cell.tracked = dayIndex >= earliest && dayIndex <= todayIndex;
  cell.read = cell.tracked && isReadDay(dayIndex);
  uint32_t seconds = 0;
  if (cell.tracked && dailyHistory_ && dailyHistory_->valueForDay(dayIndex, seconds)) {
    cell.exactDuration = true;
    cell.readingSeconds = seconds;
    cell.read = cell.read || seconds > 0;
  }
  cell.legacyDuration = cell.read && !cell.exactDuration;
  return cell;
}

ReadingCalendarCell ReadingCalendarModel::selectedCell() const {
  ReadingCalendarCell cell;
  if (!selectedDate_.isValid()) return cell;
  const uint32_t selected = readingStatsDayIndex(selectedDate_);
  const uint32_t today = readingStatsDayIndex(snapshot_.today);
  cell.date = selectedDate_;
  cell.inMonth = selectedDate_.year == visibleMonth_.year && selectedDate_.month == visibleMonth_.month;
  cell.today = selected == today;
  cell.selected = true;
  cell.future = selected > today;
  cell.tracked = selected >= earliestTrackedDay() && selected <= today;
  cell.read = cell.tracked && isReadDay(selected);
  if (cell.tracked && dailyHistory_ && dailyHistory_->valueForDay(selected, cell.readingSeconds)) {
    cell.exactDuration = true;
    cell.read = cell.read || cell.readingSeconds > 0;
  }
  cell.legacyDuration = cell.read && !cell.exactDuration;
  return cell;
}

ReadingCalendarMonthSummary ReadingCalendarModel::monthSummary() const {
  ReadingCalendarMonthSummary summary;
  if (!isAvailable() || !visibleMonth_.isValid()) return summary;
  uint8_t currentReadingStreak = 0;
  const uint8_t monthDays = daysInMonth(visibleMonth_.year, visibleMonth_.month);
  for (uint8_t day = 1; day <= monthDays; ++day) {
    const ReadingStatsDate date{visibleMonth_.year, visibleMonth_.month, day};
    const uint32_t dayIndex = readingStatsDayIndex(date);
    if (dayIndex > readingStatsDayIndex(snapshot_.today)) break;
    const bool read = isReadDay(dayIndex);
    uint32_t seconds = 0;
    const bool exact = dailyHistory_ && dailyHistory_->valueForDay(dayIndex, seconds);
    const bool readDay = read || (exact && seconds > 0);
    if (readDay) ++summary.readingDays;
    if (exact) {
      summary.totalSeconds = addReadingStatsSaturated(summary.totalSeconds, seconds);
      if (seconds > summary.bestDaySeconds) {
        summary.bestDaySeconds = seconds;
        summary.bestDayOfMonth = seconds > 0 ? day : 0;
      }
    } else if (read) {
      summary.totalIsMinimum = true;
    }
    if (readDay) {
      summary.longestReadingStreak = std::max(summary.longestReadingStreak, ++currentReadingStreak);
    } else {
      currentReadingStreak = 0;
    }
  }
  summary.bestDayKnown = !summary.totalIsMinimum && summary.bestDayOfMonth > 0;
  return summary;
}

bool ReadingCalendarModel::canMovePrevious() const {
  if (!isAvailable() || !visibleMonth_.isValid()) return false;
  ReadingStatsDate earliest;
  if (!readingStatsDateFromDayIndex(earliestTrackedDay(), earliest)) return false;
  return compareReadingStatsDate(visibleMonth_, firstOfMonth(earliest)) > 0;
}

bool ReadingCalendarModel::canMoveNext() const {
  return isAvailable() && visibleMonth_.isValid() &&
         compareReadingStatsDate(visibleMonth_, firstOfMonth(snapshot_.today)) < 0;
}

bool ReadingCalendarModel::moveMonth(const int delta) {
  if (!visibleMonth_.isValid() || delta == 0) return false;
  int year = visibleMonth_.year;
  int month = static_cast<int>(visibleMonth_.month) + delta;
  while (month < 1) {
    month += 12;
    --year;
  }
  while (month > 12) {
    month -= 12;
    ++year;
  }
  if (year < 2000 || year > 2099) return false;
  const ReadingStatsDate candidate{static_cast<uint16_t>(year), static_cast<uint8_t>(month), 1};
  ReadingStatsDate earliest;
  if (!readingStatsDateFromDayIndex(earliestTrackedDay(), earliest)) return false;
  if (compareReadingStatsDate(candidate, firstOfMonth(earliest)) < 0 ||
      compareReadingStatsDate(candidate, firstOfMonth(snapshot_.today)) > 0) {
    return false;
  }
  visibleMonth_ = candidate;
  if (selectedDate_.isValid()) {
    selectedDate_ = {candidate.year, candidate.month,
                     std::min(selectedDate_.day, daysInMonth(candidate.year, candidate.month))};
    const uint32_t earliestIndex = earliestTrackedDay();
    const uint32_t today = readingStatsDayIndex(snapshot_.today);
    uint32_t selected = readingStatsDayIndex(selectedDate_);
    if (selected < earliestIndex) readingStatsDateFromDayIndex(earliestIndex, selectedDate_);
    if (selected > today) readingStatsDateFromDayIndex(today, selectedDate_);
  }
  return true;
}

bool ReadingCalendarModel::movePrevious() {
  if (!canMovePrevious()) return false;
  return moveMonth(-1);
}

bool ReadingCalendarModel::moveNext() {
  if (!canMoveNext()) return false;
  return moveMonth(1);
}

bool ReadingCalendarModel::moveSelectedDay(const int delta) {
  if (!isAvailable() || !selectedDate_.isValid() || delta == 0) return false;
  const int64_t current = readingStatsDayIndex(selectedDate_);
  const int64_t earliest = earliestTrackedDay();
  const int64_t today = readingStatsDayIndex(snapshot_.today);
  const int64_t target = std::clamp<int64_t>(current + delta, earliest, today);
  if (target == current || !readingStatsDateFromDayIndex(static_cast<uint32_t>(target), selectedDate_)) return false;
  visibleMonth_ = firstOfMonth(selectedDate_);
  return true;
}
