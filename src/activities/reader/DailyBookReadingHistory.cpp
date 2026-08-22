#include "DailyBookReadingHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr char LOG_TAG[] = "DAYBOOK";
constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'D', 'B'};
constexpr size_t HEADER_SIZE = 12;
constexpr size_t RECORD_FIXED_SIZE = sizeof(uint32_t) + sizeof(uint16_t) * 2;
constexpr size_t MAX_FILE_SIZE =
    HEADER_SIZE +
    DailyBookReadingDay::MAX_BOOKS *
        (RECORD_FIXED_SIZE + DailyBookReadingHistory::MAX_PATH_BYTES + DailyBookReadingHistory::MAX_TITLE_BYTES) +
    sizeof(uint32_t);

enum class PathStatus : uint8_t { Missing, Valid, Invalid, NewerVersion, IoError };

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8U;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8U |
         static_cast<uint32_t>(data[offset + 2]) << 16U | static_cast<uint32_t>(data[offset + 3]) << 24U;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
  data[offset + 2] = static_cast<uint8_t>(value >> 16U);
  data[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

PathStatus readPath(const char* path, const uint32_t expectedDay, DailyBookReadingDay* out = nullptr) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    return Storage.exists(path) ? PathStatus::IoError : PathStatus::Missing;
  }
  const size_t size = file.fileSize();
  if (size < HEADER_SIZE + sizeof(uint32_t) || size > MAX_FILE_SIZE) {
    file.close();
    return size > MAX_FILE_SIZE ? PathStatus::IoError : PathStatus::Invalid;
  }
  auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
  if (!bytes) {
    file.close();
    return PathStatus::IoError;
  }
  if (file.read(bytes.get(), size) != static_cast<int>(size) || !file.close()) return PathStatus::IoError;
  if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes.get())) return PathStatus::Invalid;
  if (bytes[4] > DailyBookReadingHistory::VERSION) return PathStatus::NewerVersion;
  if (bytes[4] != DailyBookReadingHistory::VERSION || bytes[5] != 0) return PathStatus::Invalid;
  const uint16_t count = readLe16(bytes.get(), 6);
  if (count > DailyBookReadingDay::MAX_BOOKS || readLe32(bytes.get(), 8) != expectedDay) return PathStatus::Invalid;
  const uint32_t storedCrc = readLe32(bytes.get(), size - sizeof(uint32_t));
  if (storedCrc != ReadingStatsEnvelope::crc32(bytes.get(), size - sizeof(uint32_t))) return PathStatus::Invalid;

  DailyBookReadingDay decoded;
  size_t offset = HEADER_SIZE;
  for (uint16_t index = 0; index < count; ++index) {
    if (offset > size - sizeof(uint32_t) || size - sizeof(uint32_t) - offset < RECORD_FIXED_SIZE) {
      return PathStatus::Invalid;
    }
    DailyBookReadingRecord& record = decoded.records[index];
    record.seconds = readLe32(bytes.get(), offset);
    const uint16_t pathLength = readLe16(bytes.get(), offset + sizeof(uint32_t));
    const uint16_t titleLength = readLe16(bytes.get(), offset + sizeof(uint32_t) + sizeof(uint16_t));
    offset += RECORD_FIXED_SIZE;
    if (record.seconds == 0 || record.seconds > 24U * 3600U || pathLength == 0 ||
        pathLength > DailyBookReadingHistory::MAX_PATH_BYTES ||
        titleLength > DailyBookReadingHistory::MAX_TITLE_BYTES || offset > size - sizeof(uint32_t) ||
        static_cast<size_t>(pathLength) + titleLength > size - sizeof(uint32_t) - offset) {
      return PathStatus::Invalid;
    }
    record.path.assign(reinterpret_cast<const char*>(bytes.get() + offset), pathLength);
    offset += pathLength;
    record.title.assign(reinterpret_cast<const char*>(bytes.get() + offset), titleLength);
    offset += titleLength;
    for (size_t previous = 0; previous < index; ++previous) {
      if (decoded.records[previous].path == record.path) return PathStatus::Invalid;
    }
  }
  if (offset != size - sizeof(uint32_t)) return PathStatus::Invalid;
  decoded.count = count;
  if (out) *out = std::move(decoded);
  return PathStatus::Valid;
}

bool isProtected(const PathStatus status) {
  return status == PathStatus::NewerVersion || status == PathStatus::IoError;
}

DailyBookReadingHistory::LoadStatus publicStatus(const PathStatus status) {
  switch (status) {
    case PathStatus::Valid:
      return DailyBookReadingHistory::LoadStatus::Ok;
    case PathStatus::Missing:
      return DailyBookReadingHistory::LoadStatus::Missing;
    case PathStatus::NewerVersion:
      return DailyBookReadingHistory::LoadStatus::NewerVersion;
    case PathStatus::IoError:
      return DailyBookReadingHistory::LoadStatus::IoError;
    case PathStatus::Invalid:
    default:
      return DailyBookReadingHistory::LoadStatus::Invalid;
  }
}

bool saveDay(const uint32_t day, const DailyBookReadingDay& data, const PathStatus primaryStatus) {
  size_t size = HEADER_SIZE + sizeof(uint32_t);
  for (size_t index = 0; index < data.count; ++index) {
    size += RECORD_FIXED_SIZE + data.records[index].path.size() + data.records[index].title.size();
  }
  if (size > MAX_FILE_SIZE) return false;
  auto encoded = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
  if (!encoded) return false;
  memset(encoded.get(), 0, size);
  std::copy(MAGIC.begin(), MAGIC.end(), encoded.get());
  encoded[4] = DailyBookReadingHistory::VERSION;
  writeLe16(encoded.get(), 6, static_cast<uint16_t>(data.count));
  writeLe32(encoded.get(), 8, day);
  size_t offset = HEADER_SIZE;
  for (size_t index = 0; index < data.count; ++index) {
    const DailyBookReadingRecord& record = data.records[index];
    writeLe32(encoded.get(), offset, record.seconds);
    writeLe16(encoded.get(), offset + sizeof(uint32_t), static_cast<uint16_t>(record.path.size()));
    writeLe16(encoded.get(), offset + sizeof(uint32_t) + sizeof(uint16_t), static_cast<uint16_t>(record.title.size()));
    offset += RECORD_FIXED_SIZE;
    memcpy(encoded.get() + offset, record.path.data(), record.path.size());
    offset += record.path.size();
    memcpy(encoded.get() + offset, record.title.data(), record.title.size());
    offset += record.title.size();
  }
  writeLe32(encoded.get(), size - sizeof(uint32_t),
            ReadingStatsEnvelope::crc32(encoded.get(), size - sizeof(uint32_t)));

  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(DailyBookReadingHistory::DIRECTORY) && !Storage.mkdir(DailyBookReadingHistory::DIRECTORY))) {
    return false;
  }
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const std::string backup = path + ".bak";
  return ReadingStatsStorage::writeAtomic(path.c_str(), backup.c_str(), primaryStatus == PathStatus::Valid,
                                          encoded.get(), size);
}
}  // namespace

std::string DailyBookReadingHistory::pathForDay(const uint32_t day) {
  char path[64];
  snprintf(path, sizeof(path), "%s/%lu.bin", DIRECTORY, static_cast<unsigned long>(day));
  return path;
}

bool DailyBookReadingHistory::dayFromFileName(const char* name, uint32_t& day) {
  if (!name) return false;
  const size_t length = strlen(name);
  if (length < 5 || strcmp(name + length - 4, ".bin") != 0) return false;
  const size_t digitCount = length - 4;
  if (digitCount > 1 && name[0] == '0') return false;
  uint32_t parsed = 0;
  for (size_t index = 0; index < digitCount; ++index) {
    if (name[index] < '0' || name[index] > '9') return false;
    const uint8_t digit = static_cast<uint8_t>(name[index] - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(parsed, date)) return false;
  day = parsed;
  return true;
}

DailyBookReadingHistory::LoadStatus DailyBookReadingHistory::load(const uint32_t day, DailyBookReadingDay& out) {
  out = {};
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date)) return LoadStatus::Invalid;
  const std::string path = pathForDay(day);
  PathStatus status = readPath(path.c_str(), day, &out);
  if (status == PathStatus::Valid || isProtected(status)) return publicStatus(status);
  bool invalid = status == PathStatus::Invalid;
  status = readPath((path + ".bak").c_str(), day, &out);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredBackup;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  status = readPath((path + ".tmp").c_str(), day, &out);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredTemp;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  return invalid ? LoadStatus::Invalid : LoadStatus::Missing;
}

bool DailyBookReadingHistory::record(const uint32_t day, const std::string& path, const std::string& title,
                                     const uint32_t seconds) {
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date) || path.empty() || path.size() > MAX_PATH_BYTES || seconds == 0) {
    return false;
  }
  const std::string storedTitle = title.size() <= MAX_TITLE_BYTES ? title : std::string{};
  DailyBookReadingDay data;
  const LoadStatus status = load(day, data);
  if (status == LoadStatus::NewerVersion || status == LoadStatus::IoError) return false;
  auto found = std::find_if(data.records.begin(), data.records.begin() + data.count,
                            [&path](const DailyBookReadingRecord& record) { return record.path == path; });
  if (found != data.records.begin() + data.count) {
    found->seconds = std::min<uint32_t>(24U * 3600U, addReadingStatsSaturated(found->seconds, seconds));
    if (!storedTitle.empty()) found->title = storedTitle;
  } else {
    if (data.count >= DailyBookReadingDay::MAX_BOOKS) return false;
    data.records[data.count++] = {path, storedTitle, std::min<uint32_t>(seconds, 24U * 3600U)};
  }
  const std::string primaryPath = pathForDay(day);
  const PathStatus primaryStatus = readPath(primaryPath.c_str(), day);
  if (isProtected(primaryStatus)) return false;
  return saveDay(day, data, primaryStatus);
}

bool DailyBookReadingHistory::record(const std::string& path, const std::string& title,
                                     const DailyReadingHistoryDelta& delta) {
  if (delta.empty()) return true;
  if (delta.overflowed()) return false;
  for (size_t index = 0; index < delta.count(); ++index) {
    if (!record(delta.day(index), path, title, delta.seconds(index))) {
      LOG_ERR(LOG_TAG, "Could not save per-book reading history for day %lu",
              static_cast<unsigned long>(delta.day(index)));
      return false;
    }
  }
  return true;
}

bool DailyBookReadingHistory::reset() {
  return !Storage.exists(DIRECTORY) || (Storage.removeDir(DIRECTORY) && !Storage.exists(DIRECTORY));
}
