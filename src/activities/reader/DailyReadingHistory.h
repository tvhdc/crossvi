#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

class DailyReadingHistoryDelta {
 public:
  static constexpr size_t MAX_DAYS = 8;

  void recordSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  void merge(const DailyReadingHistoryDelta& other);
  void clear();
  bool empty() const { return count_ == 0; }
  bool overflowed() const { return overflowed_; }
  size_t count() const { return count_; }
  uint32_t day(size_t index) const { return days_[index]; }
  uint32_t seconds(size_t index) const { return seconds_[index]; }

 private:
  void add(uint32_t day, uint32_t seconds);

  std::array<uint32_t, MAX_DAYS> days_{};
  std::array<uint32_t, MAX_DAYS> seconds_{};
  size_t count_ = 0;
  bool overflowed_ = false;
};

class DailyReadingHistory {
 public:
  static constexpr uint32_t UNKNOWN_SECONDS = UINT32_MAX;
  static constexpr size_t DAY_COUNT = READING_HISTORY_DAYS;
  static constexpr size_t FILE_SIZE = 12 + DAY_COUNT * sizeof(uint32_t) + sizeof(uint32_t);

  enum class LoadStatus : uint8_t {
    Ok,
    Missing,
    RecoveredBackup,
    RecoveredTemp,
    Invalid,
    NewerVersion,
    IoError,
  };

  enum class BackupResult : uint8_t { Ok, Missing, Invalid, NewerVersion, IoError, Protected };

  DailyReadingHistory();

  static LoadStatus load(DailyReadingHistory& history);
  static bool reset();
  static BackupResult createBackup();
  static BackupResult inspectBackup();
  static BackupResult restoreBackup();

  bool save() const;
  bool apply(const DailyReadingHistoryDelta& delta);
  // Bring one trusted exact-day summary into the sidecar. Returns true only
  // when the in-memory history changed.
  bool reconcileExactDay(uint32_t day, uint32_t seconds);
  bool valueForDay(uint32_t day, uint32_t& seconds) const;
  bool valueForDate(const ReadingStatsDate& date, uint32_t& seconds) const;
  bool hasAnchor() const { return hasAnchor_; }
  uint32_t anchorDay() const { return anchorDay_; }

  // Used by the codec and when migrating the exact latest-day summary that
  // predates the 730-day sidecar.
  void seedExactDay(uint32_t day, uint32_t seconds);

 private:
  void advanceTo(uint32_t day);

  bool hasAnchor_ = false;
  uint32_t anchorDay_ = 0;
  std::array<uint32_t, DAY_COUNT> seconds_{};
};
