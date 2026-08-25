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
constexpr std::array<uint8_t, 4> REKEY_MAGIC = {'C', 'V', 'D', 'M'};
constexpr uint8_t REKEY_VERSION = 1;
constexpr char REKEY_PATH[] = "/.crosspoint/daily_books.move";
constexpr char REKEY_BACKUP_PATH[] = "/.crosspoint/daily_books.move.bak";
constexpr char REKEY_TEMP_PATH[] = "/.crosspoint/daily_books.move.tmp";
constexpr size_t HEADER_SIZE = 12;
constexpr size_t RECORD_FIXED_SIZE = sizeof(uint32_t) + sizeof(uint16_t) * 2;
constexpr size_t REKEY_HEADER_SIZE = REKEY_MAGIC.size() + 1 + 1 + sizeof(uint16_t) * 2;
constexpr size_t REKEY_MAX_SIZE = REKEY_HEADER_SIZE + DailyBookReadingHistory::MAX_PATH_BYTES * 2 + sizeof(uint32_t);
constexpr size_t MAX_FILE_SIZE =
    HEADER_SIZE +
    DailyBookReadingDay::MAX_BOOKS *
        (RECORD_FIXED_SIZE + DailyBookReadingHistory::MAX_PATH_BYTES + DailyBookReadingHistory::MAX_TITLE_BYTES) +
    sizeof(uint32_t);

enum class PathStatus : uint8_t { Missing, Valid, Invalid, NewerVersion, IoError };

struct RekeyIdentity {
  std::string oldPath;
  std::string newPath;
};

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

PathStatus readRekeyPath(const char* path, RekeyIdentity* identity = nullptr) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    return Storage.exists(path) ? PathStatus::IoError : PathStatus::Missing;
  }
  const size_t size = file.fileSize();
  if (size < REKEY_HEADER_SIZE + sizeof(uint32_t) || size > REKEY_MAX_SIZE) {
    file.close();
    return size > REKEY_MAX_SIZE ? PathStatus::IoError : PathStatus::Invalid;
  }
  auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
  if (!bytes) {
    file.close();
    return PathStatus::IoError;
  }
  if (file.read(bytes.get(), size) != static_cast<int>(size) || !file.close()) return PathStatus::IoError;
  if (!std::equal(REKEY_MAGIC.begin(), REKEY_MAGIC.end(), bytes.get())) return PathStatus::Invalid;
  if (bytes[4] > REKEY_VERSION) return PathStatus::NewerVersion;
  if (bytes[4] != REKEY_VERSION || bytes[5] != 0) return PathStatus::Invalid;
  const uint16_t oldLength = readLe16(bytes.get(), 6);
  const uint16_t newLength = readLe16(bytes.get(), 8);
  if (oldLength == 0 || newLength == 0 || oldLength > DailyBookReadingHistory::MAX_PATH_BYTES ||
      newLength > DailyBookReadingHistory::MAX_PATH_BYTES ||
      REKEY_HEADER_SIZE + static_cast<size_t>(oldLength) + newLength + sizeof(uint32_t) != size) {
    return PathStatus::Invalid;
  }
  const uint32_t expectedCrc = readLe32(bytes.get(), size - sizeof(uint32_t));
  if (expectedCrc != ReadingStatsEnvelope::crc32(bytes.get(), size - sizeof(uint32_t))) return PathStatus::Invalid;
  if (identity) {
    identity->oldPath.assign(reinterpret_cast<const char*>(bytes.get() + REKEY_HEADER_SIZE), oldLength);
    identity->newPath.assign(reinterpret_cast<const char*>(bytes.get() + REKEY_HEADER_SIZE + oldLength), newLength);
    if (identity->oldPath == identity->newPath) return PathStatus::Invalid;
  }
  return PathStatus::Valid;
}

PathStatus loadRekeyIdentity(RekeyIdentity& identity) {
  PathStatus status = readRekeyPath(REKEY_PATH, &identity);
  if (status == PathStatus::Valid || isProtected(status)) return status;
  bool invalid = status == PathStatus::Invalid;
  status = readRekeyPath(REKEY_BACKUP_PATH, &identity);
  if (status == PathStatus::Valid || isProtected(status)) return status;
  invalid = invalid || status == PathStatus::Invalid;
  status = readRekeyPath(REKEY_TEMP_PATH, &identity);
  if (status == PathStatus::Valid || isProtected(status)) return status;
  return invalid || status == PathStatus::Invalid ? PathStatus::Invalid : PathStatus::Missing;
}

bool removeRekeyArtifacts() {
  constexpr std::array paths = {REKEY_TEMP_PATH, REKEY_BACKUP_PATH, REKEY_PATH};
  return std::all_of(paths.begin(), paths.end(),
                     [](const char* path) { return !Storage.exists(path) || Storage.remove(path); });
}

bool saveRekeyIdentity(const RekeyIdentity& identity, const PathStatus primaryStatus) {
  const size_t size = REKEY_HEADER_SIZE + identity.oldPath.size() + identity.newPath.size() + sizeof(uint32_t);
  if (size > REKEY_MAX_SIZE) return false;
  auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
  if (!bytes) return false;
  memset(bytes.get(), 0, size);
  std::copy(REKEY_MAGIC.begin(), REKEY_MAGIC.end(), bytes.get());
  bytes[4] = REKEY_VERSION;
  writeLe16(bytes.get(), 6, static_cast<uint16_t>(identity.oldPath.size()));
  writeLe16(bytes.get(), 8, static_cast<uint16_t>(identity.newPath.size()));
  memcpy(bytes.get() + REKEY_HEADER_SIZE, identity.oldPath.data(), identity.oldPath.size());
  memcpy(bytes.get() + REKEY_HEADER_SIZE + identity.oldPath.size(), identity.newPath.data(), identity.newPath.size());
  writeLe32(bytes.get(), size - sizeof(uint32_t), ReadingStatsEnvelope::crc32(bytes.get(), size - sizeof(uint32_t)));
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  return ReadingStatsStorage::writeAtomic(REKEY_PATH, REKEY_BACKUP_PATH, primaryStatus == PathStatus::Valid,
                                          bytes.get(), size);
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

bool rekeyDay(const uint32_t day, const RekeyIdentity& identity) {
  DailyBookReadingDay data;
  const DailyBookReadingHistory::LoadStatus status = DailyBookReadingHistory::load(day, data);
  if (status == DailyBookReadingHistory::LoadStatus::Missing) return true;
  if (status == DailyBookReadingHistory::LoadStatus::NewerVersion ||
      status == DailyBookReadingHistory::LoadStatus::IoError ||
      status == DailyBookReadingHistory::LoadStatus::Invalid) {
    return false;
  }
  auto oldRecord =
      std::find_if(data.records.begin(), data.records.begin() + data.count,
                   [&identity](const DailyBookReadingRecord& record) { return record.path == identity.oldPath; });
  if (oldRecord == data.records.begin() + data.count) return true;
  auto newRecord =
      std::find_if(data.records.begin(), data.records.begin() + data.count,
                   [&identity](const DailyBookReadingRecord& record) { return record.path == identity.newPath; });
  if (newRecord != data.records.begin() + data.count) {
    newRecord->seconds =
        std::min<uint32_t>(24U * 3600U, addReadingStatsSaturated(newRecord->seconds, oldRecord->seconds));
    if (newRecord->title.empty()) newRecord->title = oldRecord->title;
    const size_t oldIndex = static_cast<size_t>(std::distance(data.records.begin(), oldRecord));
    for (size_t index = oldIndex + 1; index < data.count; ++index) {
      data.records[index - 1] = std::move(data.records[index]);
    }
    data.records[--data.count] = {};
  } else {
    oldRecord->path = identity.newPath;
  }
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const PathStatus primaryStatus = readPath(path.c_str(), day);
  return !isProtected(primaryStatus) && saveDay(day, data, primaryStatus);
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

DailyBookReadingHistory::RecordStatus DailyBookReadingHistory::record(const uint32_t day, const std::string& path,
                                                                      const std::string& title,
                                                                      const uint32_t seconds) {
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date) || path.empty() || path.size() > MAX_PATH_BYTES || seconds == 0) {
    return RecordStatus::IoError;
  }
  const std::string storedTitle = title.size() <= MAX_TITLE_BYTES ? title : std::string{};
  DailyBookReadingDay data;
  const LoadStatus status = load(day, data);
  if (status == LoadStatus::NewerVersion) return RecordStatus::Protected;
  if (status == LoadStatus::IoError) return RecordStatus::IoError;
  auto found = std::find_if(data.records.begin(), data.records.begin() + data.count,
                            [&path](const DailyBookReadingRecord& record) { return record.path == path; });
  if (found != data.records.begin() + data.count) {
    found->seconds = std::min<uint32_t>(24U * 3600U, addReadingStatsSaturated(found->seconds, seconds));
    if (!storedTitle.empty()) found->title = storedTitle;
  } else {
    if (data.count >= DailyBookReadingDay::MAX_BOOKS) return RecordStatus::CapacityExceeded;
    data.records[data.count++] = {path, storedTitle, std::min<uint32_t>(seconds, 24U * 3600U)};
  }
  const std::string primaryPath = pathForDay(day);
  const PathStatus primaryStatus = readPath(primaryPath.c_str(), day);
  if (primaryStatus == PathStatus::NewerVersion) return RecordStatus::Protected;
  if (primaryStatus == PathStatus::IoError) return RecordStatus::IoError;
  return saveDay(day, data, primaryStatus) ? RecordStatus::Ok : RecordStatus::IoError;
}

DailyBookReadingHistory::RecordStatus DailyBookReadingHistory::record(const std::string& path, const std::string& title,
                                                                      const DailyReadingHistoryDelta& delta) {
  if (delta.empty()) return RecordStatus::Ok;
  if (delta.overflowed()) return RecordStatus::IoError;
  RecordStatus aggregate = RecordStatus::Ok;
  for (size_t index = 0; index < delta.count(); ++index) {
    const RecordStatus status = record(delta.day(index), path, title, delta.seconds(index));
    if (status != RecordStatus::Ok) {
      LOG_ERR(LOG_TAG, "Could not save per-book reading history for day %lu",
              static_cast<unsigned long>(delta.day(index)));
      if (status == RecordStatus::Protected || status == RecordStatus::IoError || aggregate == RecordStatus::Ok) {
        aggregate = status;
      }
    }
  }
  return aggregate;
}

bool DailyBookReadingHistory::prepareRekey(const std::string& oldPath, const std::string& newPath) {
  if (oldPath.empty() || newPath.empty() || oldPath == newPath || oldPath.size() > MAX_PATH_BYTES ||
      newPath.size() > MAX_PATH_BYTES) {
    return false;
  }
  RekeyIdentity existing;
  const PathStatus status = loadRekeyIdentity(existing);
  if (status == PathStatus::Valid) {
    if (existing.oldPath == oldPath && existing.newPath == newPath) return true;
    if (!recoverPreparedRekey()) return false;
  } else if (isProtected(status) || status == PathStatus::Invalid) {
    return false;
  }
  return saveRekeyIdentity({oldPath, newPath}, readRekeyPath(REKEY_PATH));
}

bool DailyBookReadingHistory::finishPreparedRekey() {
  RekeyIdentity identity;
  const PathStatus status = loadRekeyIdentity(identity);
  if (status == PathStatus::Missing) return true;
  if (status != PathStatus::Valid) return false;
  if (Storage.exists(DIRECTORY)) {
    HalFile directory = Storage.open(DIRECTORY);
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      return false;
    }
    char name[64]{};
    for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      if (entry.isDirectory()) {
        entry.close();
        continue;
      }
      const size_t length = entry.getName(name, sizeof(name));
      const bool closed = entry.close();
      uint32_t day = 0;
      if (!closed || length == 0 || length >= sizeof(name)) {
        directory.close();
        return false;
      }
      if (dayFromFileName(name, day) && !rekeyDay(day, identity)) {
        directory.close();
        return false;
      }
    }
    if (directory.getError() != 0 || !directory.close()) return false;
  }
  return removeRekeyArtifacts();
}

bool DailyBookReadingHistory::cancelPreparedRekey(const std::string& oldPath, const std::string& newPath) {
  RekeyIdentity identity;
  const PathStatus status = loadRekeyIdentity(identity);
  if (status == PathStatus::Missing) return true;
  if (status != PathStatus::Valid || identity.oldPath != oldPath || identity.newPath != newPath) return false;
  return removeRekeyArtifacts();
}

bool DailyBookReadingHistory::recoverPreparedRekey() {
  RekeyIdentity identity;
  const PathStatus status = loadRekeyIdentity(identity);
  if (status == PathStatus::Missing) return true;
  if (status != PathStatus::Valid) return false;
  const bool oldExists = Storage.exists(identity.oldPath.c_str());
  const bool newExists = Storage.exists(identity.newPath.c_str());
  if (oldExists == newExists) return false;
  return oldExists ? removeRekeyArtifacts() : finishPreparedRekey();
}

bool DailyBookReadingHistory::pendingRekeyAlias(const std::string& path, std::string& alias) {
  alias.clear();
  RekeyIdentity identity;
  if (loadRekeyIdentity(identity) != PathStatus::Valid) return false;
  if (path == identity.oldPath) {
    alias = identity.newPath;
  } else if (path == identity.newPath) {
    alias = identity.oldPath;
  }
  return !alias.empty();
}

bool DailyBookReadingHistory::canReset() {
  if (!recoverPreparedRekey()) return false;
  if (!Storage.exists(DIRECTORY)) return true;
  HalFile directory = Storage.open(DIRECTORY);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }
  char name[64]{};
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t length = entry.getName(name, sizeof(name));
    const bool closed = entry.close();
    uint32_t day = 0;
    if (!closed || length == 0 || length >= sizeof(name)) {
      directory.close();
      return false;
    }
    if (isDirectory) continue;
    size_t canonicalLength = length;
    if (canonicalLength > 4 && strcmp(name + canonicalLength - 4, ".bak") == 0) {
      canonicalLength -= 4;
      name[canonicalLength] = '\0';
    } else if (canonicalLength > 4 && strcmp(name + canonicalLength - 4, ".tmp") == 0) {
      canonicalLength -= 4;
      name[canonicalLength] = '\0';
    }
    if (!dayFromFileName(name, day)) continue;
    DailyBookReadingDay data;
    const LoadStatus status = load(day, data);
    if (status == LoadStatus::NewerVersion || status == LoadStatus::IoError) {
      directory.close();
      return false;
    }
  }
  return directory.getError() == 0 && directory.close();
}

bool DailyBookReadingHistory::reset() {
  if (!canReset()) return false;
  const bool directoryReset =
      !Storage.exists(DIRECTORY) || (Storage.removeDir(DIRECTORY) && !Storage.exists(DIRECTORY));
  return directoryReset && removeRekeyArtifacts();
}
