#include "DailyReadingHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"

namespace {
constexpr char LOG_TAG[] = "DAYHIST";
constexpr char HISTORY_PATH[] = "/.crosspoint/daily_history_v1.bin";
constexpr char HISTORY_BACKUP_PATH[] = "/.crosspoint/daily_history_v1.bin.bak";
constexpr char HISTORY_TEMP_PATH[] = "/.crosspoint/daily_history_v1.bin.tmp";
constexpr char USER_BACKUP_DIRECTORY[] = "/.crosspoint/stats_backups";
constexpr char USER_BACKUP_PATH[] = "/.crosspoint/stats_backups/daily_history_v1.bin";
constexpr char USER_BACKUP_PREVIOUS_PATH[] = "/.crosspoint/stats_backups/daily_history_v1.bin.bak";
constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'D', 'H'};
constexpr uint8_t VERSION = 2;
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t FLAG_HAS_ANCHOR = 1;
constexpr size_t HEADER_SIZE = 12;

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

enum class PathStatus : uint8_t { Missing, Valid, Invalid, NewerVersion, IoError };

PathStatus readPath(const char* path, DailyReadingHistory* history = nullptr) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    return Storage.exists(path) ? PathStatus::IoError : PathStatus::Missing;
  }

  const size_t fileSize = file.fileSize();
  std::array<uint8_t, HEADER_SIZE> header{};
  if (fileSize < header.size() || file.read(header.data(), header.size()) != static_cast<int>(header.size())) {
    file.close();
    return PathStatus::Invalid;
  }
  if (!std::equal(MAGIC.begin(), MAGIC.end(), header.begin())) {
    file.close();
    return PathStatus::Invalid;
  }
  const uint8_t version = header[4];
  if (version > VERSION) {
    file.close();
    return PathStatus::NewerVersion;
  }
  const size_t expectedSize =
      version == LEGACY_VERSION ? DailyReadingHistory::LEGACY_FILE_SIZE : DailyReadingHistory::FILE_SIZE;
  if ((version != LEGACY_VERSION && version != VERSION) ||
      readLe16(header.data(), 6) != DailyReadingHistory::DAY_COUNT || fileSize != expectedSize) {
    file.close();
    return PathStatus::Invalid;
  }

  auto encoded = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[expectedSize]);
  if (!encoded) {
    file.close();
    return PathStatus::IoError;
  }
  memcpy(encoded.get(), header.data(), header.size());
  const size_t remaining = expectedSize - header.size();
  if (file.read(encoded.get() + header.size(), remaining) != static_cast<int>(remaining) || !file.close()) {
    return PathStatus::IoError;
  }
  const uint32_t storedCrc = readLe32(encoded.get(), expectedSize - sizeof(uint32_t));
  if (storedCrc != ReadingStatsEnvelope::crc32(encoded.get(), expectedSize - sizeof(uint32_t))) {
    return PathStatus::Invalid;
  }

  if ((header[5] & ~FLAG_HAS_ANCHOR) != 0) return PathStatus::Invalid;
  uint32_t anchor = 0;
  if ((header[5] & FLAG_HAS_ANCHOR) != 0) {
    ReadingStatsDate date;
    anchor = readLe32(header.data(), 8);
    if (!readingStatsDateFromDayIndex(anchor, date)) return PathStatus::Invalid;
  }
  for (size_t index = 0; index < DailyReadingHistory::DAY_COUNT; ++index) {
    const uint32_t value = readLe32(encoded.get(), HEADER_SIZE + index * sizeof(uint32_t));
    if (value != DailyReadingHistory::UNKNOWN_SECONDS && value > 24u * 3600u) {
      return PathStatus::Invalid;
    }
  }
  if (history) {
    *history = DailyReadingHistory{};
    if ((header[5] & FLAG_HAS_ANCHOR) != 0) {
      history->seedExactDay(anchor, 0);
      for (size_t index = 0; index < DailyReadingHistory::DAY_COUNT && index <= anchor; ++index) {
        history->seedExactDay(anchor - static_cast<uint32_t>(index),
                              readLe32(encoded.get(), HEADER_SIZE + index * sizeof(uint32_t)));
      }
    }
    if (version == VERSION) {
      history->raiseLifetimeReadingDaysTo(
          readLe32(encoded.get(), HEADER_SIZE + DailyReadingHistory::DAY_COUNT * sizeof(uint32_t)));
    }
  }
  return PathStatus::Valid;
}

bool isProtected(const PathStatus status) {
  return status == PathStatus::NewerVersion || status == PathStatus::IoError;
}

DailyReadingHistory::LoadStatus publicStatus(const PathStatus status) {
  switch (status) {
    case PathStatus::NewerVersion:
      return DailyReadingHistory::LoadStatus::NewerVersion;
    case PathStatus::IoError:
      return DailyReadingHistory::LoadStatus::IoError;
    case PathStatus::Invalid:
      return DailyReadingHistory::LoadStatus::Invalid;
    case PathStatus::Missing:
    case PathStatus::Valid:
    default:
      return DailyReadingHistory::LoadStatus::Missing;
  }
}

DailyReadingHistory::BackupResult backupResult(const PathStatus status) {
  switch (status) {
    case PathStatus::Missing:
      return DailyReadingHistory::BackupResult::Missing;
    case PathStatus::NewerVersion:
      return DailyReadingHistory::BackupResult::NewerVersion;
    case PathStatus::IoError:
      return DailyReadingHistory::BackupResult::IoError;
    case PathStatus::Invalid:
      return DailyReadingHistory::BackupResult::Invalid;
    case PathStatus::Valid:
    default:
      return DailyReadingHistory::BackupResult::Ok;
  }
}
}  // namespace

void DailyReadingHistoryDelta::add(const uint32_t day, const uint32_t seconds) {
  if (seconds == 0) return;
  for (size_t index = 0; index < count_; ++index) {
    if (days_[index] == day) {
      seconds_[index] = addReadingStatsSaturated(seconds_[index], seconds);
      return;
    }
  }
  if (count_ >= MAX_DAYS) {
    overflowed_ = true;
    return;
  }
  days_[count_] = day;
  seconds_[count_] = seconds;
  ++count_;
}

void DailyReadingHistoryDelta::recordSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0 && !overflowed_) {
    const uint32_t secondOfDay =
        static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second;
    const uint32_t segment = std::min(remaining, 24u * 3600u - secondOfDay);
    add(readingStatsDayIndex(cursor.date), segment);
    if (overflowed_) break;
    remaining -= segment;
    const uint32_t previousDay = readingStatsDayIndex(cursor.date);
    addSecondsToReadingStatsDateTime(cursor, segment);
    if (remaining > 0 && previousDay == readingStatsDayIndex(cursor.date)) {
      overflowed_ = true;
      break;
    }
  }
}

void DailyReadingHistoryDelta::merge(const DailyReadingHistoryDelta& other) {
  for (size_t index = 0; index < other.count_; ++index) add(other.days_[index], other.seconds_[index]);
  overflowed_ = overflowed_ || other.overflowed_;
}

void DailyReadingHistoryDelta::clear() {
  days_.fill(0);
  seconds_.fill(0);
  count_ = 0;
  overflowed_ = false;
}

DailyReadingHistory::DailyReadingHistory() { seconds_.fill(UNKNOWN_SECONDS); }

DailyReadingHistory::LoadStatus DailyReadingHistory::load(DailyReadingHistory& history) {
  history = {};
  PathStatus status = readPath(HISTORY_PATH, &history);
  if (status == PathStatus::Valid) return LoadStatus::Ok;
  if (isProtected(status)) return publicStatus(status);
  bool invalid = status == PathStatus::Invalid;
  status = readPath(HISTORY_BACKUP_PATH, &history);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredBackup;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  status = readPath(HISTORY_TEMP_PATH, &history);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredTemp;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  return invalid ? LoadStatus::Invalid : LoadStatus::Missing;
}

bool DailyReadingHistory::save() const {
  const PathStatus primaryStatus = readPath(HISTORY_PATH);
  if (isProtected(primaryStatus)) return false;
  if (isProtected(readPath(HISTORY_BACKUP_PATH)) || isProtected(readPath(HISTORY_TEMP_PATH))) return false;

  auto encoded = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[FILE_SIZE]);
  if (!encoded) return false;
  memset(encoded.get(), 0, FILE_SIZE);
  std::copy(MAGIC.begin(), MAGIC.end(), encoded.get());
  encoded[4] = VERSION;
  encoded[5] = hasAnchor_ ? FLAG_HAS_ANCHOR : 0;
  writeLe16(encoded.get(), 6, DAY_COUNT);
  writeLe32(encoded.get(), 8, anchorDay_);
  for (size_t index = 0; index < DAY_COUNT; ++index) {
    writeLe32(encoded.get(), HEADER_SIZE + index * sizeof(uint32_t), seconds_[index]);
  }
  writeLe32(encoded.get(), HEADER_SIZE + DAY_COUNT * sizeof(uint32_t), lifetimeReadingDays_);
  writeLe32(encoded.get(), FILE_SIZE - sizeof(uint32_t),
            ReadingStatsEnvelope::crc32(encoded.get(), FILE_SIZE - sizeof(uint32_t)));
  return ReadingStatsStorage::writeAtomic(HISTORY_PATH, HISTORY_BACKUP_PATH, primaryStatus == PathStatus::Valid,
                                          encoded.get(), FILE_SIZE);
}

void DailyReadingHistory::advanceTo(const uint32_t day) {
  if (!hasAnchor_) {
    hasAnchor_ = true;
    anchorDay_ = day;
    seconds_.fill(UNKNOWN_SECONDS);
    return;
  }
  if (day <= anchorDay_) return;
  const uint32_t shift = day - anchorDay_;
  if (shift >= DAY_COUNT) {
    seconds_.fill(UNKNOWN_SECONDS);
  } else {
    for (size_t index = DAY_COUNT; index-- > shift;) seconds_[index] = seconds_[index - shift];
    std::fill(seconds_.begin(), seconds_.begin() + shift, 0);
  }
  anchorDay_ = day;
}

void DailyReadingHistory::seedExactDay(const uint32_t day, const uint32_t seconds) {
  if (seconds != UNKNOWN_SECONDS && seconds > 24u * 3600u) return;
  if (!hasAnchor_ || day > anchorDay_) advanceTo(day);
  if (!hasAnchor_ || day > anchorDay_ || anchorDay_ - day >= DAY_COUNT) return;
  uint32_t& current = seconds_[anchorDay_ - day];
  if ((current == 0 || current == UNKNOWN_SECONDS) && seconds != 0 && seconds != UNKNOWN_SECONDS) {
    lifetimeReadingDays_ = addReadingStatsSaturated(lifetimeReadingDays_, 1);
  }
  current = seconds;
}

void DailyReadingHistory::raiseLifetimeReadingDaysTo(const uint32_t days) {
  lifetimeReadingDays_ = std::max(lifetimeReadingDays_, days);
}

bool DailyReadingHistory::apply(const DailyReadingHistoryDelta& delta) {
  if (delta.overflowed()) return false;
  for (size_t index = 0; index < delta.count(); ++index) {
    const uint32_t day = delta.day(index);
    advanceTo(day);
    if (!hasAnchor_ || day > anchorDay_ || anchorDay_ - day >= DAY_COUNT) continue;
    uint32_t& value = seconds_[anchorDay_ - day];
    if (value == 0 || value == UNKNOWN_SECONDS) {
      value = 0;
      lifetimeReadingDays_ = addReadingStatsSaturated(lifetimeReadingDays_, 1);
    }
    value = std::min<uint32_t>(24u * 3600u, addReadingStatsSaturated(value, delta.seconds(index)));
  }
  return true;
}

bool DailyReadingHistory::reconcileExactDay(const uint32_t day, const uint32_t seconds) {
  uint32_t current = 0;
  if (valueForDay(day, current) && current == seconds) return false;
  seedExactDay(day, seconds);
  return valueForDay(day, current) && current == seconds;
}

bool DailyReadingHistory::valueForDay(const uint32_t day, uint32_t& seconds) const {
  if (!hasAnchor_ || day > anchorDay_ || anchorDay_ - day >= DAY_COUNT) return false;
  const uint32_t value = seconds_[anchorDay_ - day];
  if (value == UNKNOWN_SECONDS) return false;
  seconds = value;
  return true;
}

bool DailyReadingHistory::empty() const {
  if (!hasAnchor_) return true;
  return std::all_of(seconds_.begin(), seconds_.end(),
                     [](const uint32_t seconds) { return seconds == 0 || seconds == UNKNOWN_SECONDS; });
}

bool DailyReadingHistory::canReset() {
  return !isProtected(readPath(HISTORY_PATH)) && !isProtected(readPath(HISTORY_BACKUP_PATH)) &&
         !isProtected(readPath(HISTORY_TEMP_PATH));
}

bool DailyReadingHistory::reset() {
  if (!canReset()) return false;
  const DailyReadingHistory cleared;
  if (!cleared.save()) return false;
  return cleared.save();
}

DailyReadingHistory::BackupResult DailyReadingHistory::createBackup() {
  auto history = std::unique_ptr<DailyReadingHistory>(new (std::nothrow) DailyReadingHistory);
  if (!history) return BackupResult::IoError;
  const LoadStatus status = load(*history);
  if (status == LoadStatus::Missing) return BackupResult::Missing;
  if (status == LoadStatus::NewerVersion) return BackupResult::NewerVersion;
  if (status == LoadStatus::IoError) return BackupResult::IoError;
  if (status == LoadStatus::Invalid) return BackupResult::Invalid;
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(USER_BACKUP_DIRECTORY) && !Storage.mkdir(USER_BACKUP_DIRECTORY))) {
    return BackupResult::IoError;
  }

  const PathStatus primaryStatus = readPath(USER_BACKUP_PATH);
  if (isProtected(primaryStatus)) return BackupResult::Protected;
  if (isProtected(readPath(USER_BACKUP_PREVIOUS_PATH))) return BackupResult::Protected;

  auto encoded = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[FILE_SIZE]);
  if (!encoded) return BackupResult::IoError;
  memset(encoded.get(), 0, FILE_SIZE);
  std::copy(MAGIC.begin(), MAGIC.end(), encoded.get());
  encoded[4] = VERSION;
  encoded[5] = history->hasAnchor_ ? FLAG_HAS_ANCHOR : 0;
  writeLe16(encoded.get(), 6, DAY_COUNT);
  writeLe32(encoded.get(), 8, history->anchorDay_);
  for (size_t index = 0; index < DAY_COUNT; ++index) {
    writeLe32(encoded.get(), HEADER_SIZE + index * sizeof(uint32_t), history->seconds_[index]);
  }
  writeLe32(encoded.get(), HEADER_SIZE + DAY_COUNT * sizeof(uint32_t), history->lifetimeReadingDays_);
  writeLe32(encoded.get(), FILE_SIZE - sizeof(uint32_t),
            ReadingStatsEnvelope::crc32(encoded.get(), FILE_SIZE - sizeof(uint32_t)));
  return ReadingStatsStorage::writeAtomic(USER_BACKUP_PATH, USER_BACKUP_PREVIOUS_PATH,
                                          primaryStatus == PathStatus::Valid, encoded.get(), FILE_SIZE)
             ? BackupResult::Ok
             : BackupResult::IoError;
}

DailyReadingHistory::BackupResult DailyReadingHistory::restoreBackup() {
  auto backup = std::unique_ptr<DailyReadingHistory>(new (std::nothrow) DailyReadingHistory);
  if (!backup) return BackupResult::IoError;
  const PathStatus status = readPath(USER_BACKUP_PATH, backup.get());
  if (status != PathStatus::Valid) return backupResult(status);
  return backup->save() ? BackupResult::Ok : BackupResult::Protected;
}

DailyReadingHistory::BackupResult DailyReadingHistory::inspectBackup() {
  return backupResult(readPath(USER_BACKUP_PATH));
}
