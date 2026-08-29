#include "VCodexStatsImporter.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

#include "BookReadingStats.h"
#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"
#include "ReadingAchievements.h"
#include "ReadingStatsCodec.h"
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr char LOG_TAG[] = "VCDXIMP";
constexpr char MARKER_PATH[] = "/.crosspoint/vcodex_stats_import_v1.bin";
constexpr char MARKER_BACKUP_PATH[] = "/.crosspoint/vcodex_stats_import_v1.bin.bak";
constexpr char MARKER_TEMP_PATH[] = "/.crosspoint/vcodex_stats_import_v1.bin.tmp";
constexpr size_t MARKER_SIZE = 20;
constexpr uint8_t MARKER_VERSION = 1;
constexpr uint32_t UNIX_DAYS_TO_2000 = 10957;
constexpr size_t MAX_SOURCE_BYTES = 4U * 1024U * 1024U;
constexpr size_t MAX_PATH_BYTES = 511;
constexpr size_t MAX_METADATA_BYTES = 255;

enum class MarkerState : uint8_t { Declined = 1, Pending = 2, Completed = 3, Failed = 4 };
enum class MarkerStatus : uint8_t { Missing, Valid, Invalid, IoError };

struct Marker {
  MarkerState state = MarkerState::Failed;
  uint32_t sourceSize = 0;
  uint32_t sourceHash = 0;
};

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

MarkerStatus loadMarkerPath(const char* path, Marker& marker) {
  std::array<uint8_t, MARKER_SIZE> data{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path, data.data(), data.size());
  if (read.result == ReadingStatsStorage::ReadResult::Missing) return MarkerStatus::Missing;
  if (read.result != ReadingStatsStorage::ReadResult::Ok) return MarkerStatus::IoError;
  if (read.size != data.size() || memcmp(data.data(), "VCIM", 4) != 0 || data[4] != MARKER_VERSION ||
      data[5] < static_cast<uint8_t>(MarkerState::Declined) || data[5] > static_cast<uint8_t>(MarkerState::Failed) ||
      readLe32(data.data(), 16) != ReadingStatsEnvelope::crc32(data.data(), 16)) {
    return MarkerStatus::Invalid;
  }
  marker.state = static_cast<MarkerState>(data[5]);
  marker.sourceSize = readLe32(data.data(), 8);
  marker.sourceHash = readLe32(data.data(), 12);
  return MarkerStatus::Valid;
}

MarkerStatus loadMarker(Marker& marker) {
  bool sawInvalid = false;
  for (const char* path : {MARKER_PATH, MARKER_BACKUP_PATH, MARKER_TEMP_PATH}) {
    Marker candidate;
    const MarkerStatus status = loadMarkerPath(path, candidate);
    if (status == MarkerStatus::Missing) continue;
    if (status == MarkerStatus::IoError) return MarkerStatus::IoError;
    if (status == MarkerStatus::Invalid) {
      sawInvalid = true;
      continue;
    }
    marker = candidate;
    return MarkerStatus::Valid;
  }
  return sawInvalid ? MarkerStatus::Invalid : MarkerStatus::Missing;
}

bool saveMarker(const MarkerState state, const uint32_t sourceSize, const uint32_t sourceHash) {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  std::array<uint8_t, MARKER_SIZE> data{};
  memcpy(data.data(), "VCIM", 4);
  data[4] = MARKER_VERSION;
  data[5] = static_cast<uint8_t>(state);
  writeLe32(data.data(), 8, sourceSize);
  writeLe32(data.data(), 12, sourceHash);
  writeLe32(data.data(), 16, ReadingStatsEnvelope::crc32(data.data(), 16));
  Marker ignored;
  const MarkerStatus primaryStatus = loadMarkerPath(MARKER_PATH, ignored);
  const MarkerStatus backupStatus = loadMarkerPath(MARKER_BACKUP_PATH, ignored);
  const MarkerStatus tempStatus = loadMarkerPath(MARKER_TEMP_PATH, ignored);
  if (primaryStatus == MarkerStatus::IoError || backupStatus == MarkerStatus::IoError ||
      tempStatus == MarkerStatus::IoError) {
    return false;
  }
  return ReadingStatsStorage::writeAtomic(MARKER_PATH, MARKER_BACKUP_PATH, primaryStatus == MarkerStatus::Valid,
                                          data.data(), data.size());
}

bool validBookPath(const std::string& path) {
  if (path.empty() || path.size() > MAX_PATH_BYTES || path.front() != '/' ||
      !(FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
        FsHelpers::hasMarkdownExtension(path))) {
    return false;
  }
  HalFile file = Storage.open(path.c_str());
  const bool valid = file && !file.isDirectory();
  if (file) file.close();
  return valid;
}

std::string cachePathForBook(const std::string& path) {
  const char* prefix = nullptr;
  if (FsHelpers::hasEpubExtension(path)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(path)) {
    prefix = "xtc_";
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    prefix = "txt_";
  } else {
    return {};
  }
  return std::string("/.crosspoint/") + prefix + std::to_string(std::hash<std::string>{}(path));
}

uint32_t saturatedSeconds(const uint64_t milliseconds) {
  return static_cast<uint32_t>(std::min<uint64_t>(milliseconds / 1000u, UINT32_MAX));
}

bool timestampToLocal(const uint32_t timestamp, const int16_t offsetMinutes, ReadingStatsDate& date,
                      uint16_t& minuteOfDay) {
  if (timestamp == 0) return false;
  int64_t localSeconds = static_cast<int64_t>(timestamp) + static_cast<int64_t>(offsetMinutes) * 60;
  if (localSeconds < 0) return false;
  const uint64_t seconds = static_cast<uint64_t>(localSeconds);
  const uint64_t unixDay = seconds / 86400u;
  if (unixDay < UNIX_DAYS_TO_2000 || unixDay - UNIX_DAYS_TO_2000 > UINT32_MAX ||
      !readingStatsDateFromDayIndex(static_cast<uint32_t>(unixDay - UNIX_DAYS_TO_2000), date)) {
    return false;
  }
  minuteOfDay = static_cast<uint16_t>(seconds % 86400u / 60u);
  return true;
}

enum class Context : uint8_t {
  Root,
  ReadingDays,
  ReadingDay,
  SessionLog,
  Session,
  Books,
  Book,
  KnownPaths,
  BookReadingDays,
  IgnoreObject,
  IgnoreArray,
};

enum class Key : uint8_t {
  Unknown,
  FormatVersion,
  ReadingDays,
  LegacyReadingDays,
  SessionLog,
  Books,
  DayOrdinal,
  ReadingMs,
  SessionMs,
  Path,
  KnownPaths,
  Title,
  Author,
  TotalReadingMs,
  Sessions,
  FirstReadAt,
  CompletedAt,
  Completed,
};

Key parseKey(const char* value, const size_t length) {
  const auto matches = [value, length](const char* expected) {
    return strlen(expected) == length && memcmp(value, expected, length) == 0;
  };
  if (matches("formatVersion")) return Key::FormatVersion;
  if (matches("readingDays")) return Key::ReadingDays;
  if (matches("legacyReadingDays")) return Key::LegacyReadingDays;
  if (matches("sessionLog")) return Key::SessionLog;
  if (matches("books")) return Key::Books;
  if (matches("dayOrdinal")) return Key::DayOrdinal;
  if (matches("readingMs")) return Key::ReadingMs;
  if (matches("sessionMs")) return Key::SessionMs;
  if (matches("path")) return Key::Path;
  if (matches("knownPaths")) return Key::KnownPaths;
  if (matches("title")) return Key::Title;
  if (matches("author")) return Key::Author;
  if (matches("totalReadingMs")) return Key::TotalReadingMs;
  if (matches("sessions")) return Key::Sessions;
  if (matches("firstReadAt")) return Key::FirstReadAt;
  if (matches("completedAt")) return Key::CompletedAt;
  if (matches("completed")) return Key::Completed;
  return Key::Unknown;
}

bool parseUnsigned(const char* value, const size_t length, uint64_t& parsed) {
  if (!value || length == 0 || length >= 32 || value[0] == '-') return false;
  char buffer[32];
  memcpy(buffer, value, length);
  buffer[length] = '\0';
  char* end = nullptr;
  errno = 0;
  const unsigned long long result = strtoull(buffer, &end, 10);
  if (errno == ERANGE || end != buffer + length) return false;
  parsed = result;
  return true;
}

struct ParsedBook {
  std::string path;
  std::string alternatePath;
  std::string title;
  std::string author;
  uint64_t totalReadingMs = 0;
  uint64_t sessions = 0;
  uint64_t firstReadAt = 0;
  uint64_t completedAt = 0;
  bool completed = false;
  bool hasTotalReadingMs = false;
  bool hasSessions = false;
  bool hasCompleted = false;
};

using BookVisitor = bool (*)(void*, const ParsedBook&);

struct ParsedDocument {
  VCodexStatsImportSummary summary;
  DailyReadingHistory history;
  GlobalReadingStats global;
  uint32_t sourceSize = 0;
  uint32_t sourceHash = 2166136261u;
};

class VCodexSaxHandler {
 public:
  VCodexSaxHandler(ParsedDocument& output, BookVisitor visitor, void* visitorContext)
      : output_(output), visitor_(visitor), visitorContext_(visitorContext) {}

  JsonCallbacks callbacks() {
    return {this,        onKeyThunk,         onStringThunk,    onNumberThunk,     onBoolThunk,
            onNullThunk, onObjectStartThunk, onObjectEndThunk, onArrayStartThunk, onArrayEndThunk};
  }

  bool valid() const {
    if (!valid_ || depth_ != 0 || !rootSeen_ || formatVersion_ == 0 || formatVersion_ > 6 ||
        (!sawReadingDays_ && !sawLegacyDays_ && !sawSessionLog_ && !sawBooks_)) {
      return false;
    }
    return true;
  }

  void finalize() {
    output_.global.importedFromVCodex = true;
    output_.global.pageTurnsUnavailable = true;
    output_.global.totalPagesTurned = 0;

    if (sawReadingDays_ && !readingDaysMissingTime_) {
      output_.global.totalReadingSeconds = saturatedSeconds(readingDaysTotalMs_);
      output_.summary.totalReadingSeconds = output_.global.totalReadingSeconds;
      output_.summary.hasReadingTime = true;
    } else if (sawBooks_ && booksMissingTime_ == 0) {
      output_.global.totalReadingSeconds = saturatedSeconds(bookTotalMs_);
      output_.summary.totalReadingSeconds = output_.global.totalReadingSeconds;
      output_.summary.hasReadingTime = true;
    } else {
      output_.global.readingTimeUnavailable = true;
    }

    if (sawBooks_ && booksMissingSessions_ == 0) {
      output_.global.totalSessions = static_cast<uint32_t>(std::min<uint64_t>(bookSessions_, UINT32_MAX));
      output_.summary.totalSessions = output_.global.totalSessions;
      output_.summary.hasSessions = true;
    } else {
      output_.global.sessionsUnavailable = true;
    }

    if (sawBooks_ && booksMissingCompletion_ == 0) {
      output_.global.completedBooks = completedBooks_;
      output_.summary.completedBooks = completedBooks_;
      output_.summary.hasCompletedBooks = true;
    } else {
      output_.global.completionUnavailable = true;
    }

    if (output_.history.hasAnchor()) {
      output_.global.readingHistoryAnchorDay = output_.history.anchorDay();
      uint16_t days = 0;
      for (size_t index = 0; index < DailyReadingHistory::DAY_COUNT; ++index) {
        const uint32_t day = output_.history.anchorDay() - static_cast<uint32_t>(index);
        uint32_t seconds = 0;
        if (output_.history.valueForDay(day, seconds) || hasUnknownDay(day)) {
          markReadingHistoryDay(output_.global.readingHistoryAnchorDay, output_.global.readingHistoryBits, day);
          ++days;
        }
      }
      output_.summary.calendarDays = days;
      output_.global.longestReadingStreak =
          computeReadingHistoryLongestStreak(output_.global.readingHistoryAnchorDay, output_.global.readingHistoryBits);
    }
    output_.summary.hasCalendarDays = sawReadingDays_;
    output_.summary.hasResolvableBooks = sawBooks_;
  }

 private:
  ParsedDocument& output_;
  BookVisitor visitor_ = nullptr;
  void* visitorContext_ = nullptr;
  std::array<Context, StreamingJsonParser::MAX_NESTING> stack_{};
  size_t depth_ = 0;
  Key key_ = Key::Unknown;
  bool valid_ = true;
  bool rootSeen_ = false;
  uint32_t formatVersion_ = 1;
  bool sawReadingDays_ = false;
  bool sawLegacyDays_ = false;
  bool sawSessionLog_ = false;
  bool sawBooks_ = false;
  bool readingDaysMissingTime_ = false;
  uint64_t readingDaysTotalMs_ = 0;
  uint64_t bookTotalMs_ = 0;
  uint64_t bookSessions_ = 0;
  uint32_t completedBooks_ = 0;
  uint32_t booksMissingTime_ = 0;
  uint32_t booksMissingSessions_ = 0;
  uint32_t booksMissingCompletion_ = 0;
  ParsedBook book_;
  uint32_t dayOrdinal_ = 0;
  uint64_t dayReadingMs_ = 0;
  bool dayHasOrdinal_ = false;
  bool dayHasReadingMs_ = false;
  std::array<uint32_t, DailyReadingHistory::DAY_COUNT> unknownDays_{};
  size_t unknownDayCount_ = 0;

  Context context() const { return depth_ == 0 ? Context::IgnoreObject : stack_[depth_ - 1]; }

  void push(const Context context) {
    if (depth_ >= stack_.size()) {
      valid_ = false;
      return;
    }
    stack_[depth_++] = context;
  }

  void rememberUnknownDay(const uint32_t day) {
    if (std::find(unknownDays_.begin(), unknownDays_.begin() + static_cast<std::ptrdiff_t>(unknownDayCount_), day) !=
        unknownDays_.begin() + static_cast<std::ptrdiff_t>(unknownDayCount_)) {
      return;
    }
    if (unknownDayCount_ < unknownDays_.size()) unknownDays_[unknownDayCount_++] = day;
  }

  bool hasUnknownDay(const uint32_t day) const {
    return std::find(unknownDays_.begin(), unknownDays_.begin() + static_cast<std::ptrdiff_t>(unknownDayCount_), day) !=
           unknownDays_.begin() + static_cast<std::ptrdiff_t>(unknownDayCount_);
  }

  void addDay(const uint32_t ordinal, const uint64_t milliseconds, const bool hasMilliseconds) {
    if (ordinal < UNIX_DAYS_TO_2000 || ordinal - UNIX_DAYS_TO_2000 > UINT32_MAX) return;
    ReadingStatsDate date;
    const uint32_t day = ordinal - UNIX_DAYS_TO_2000;
    if (!readingStatsDateFromDayIndex(day, date)) return;
    if (!hasMilliseconds) {
      rememberUnknownDay(day);
      output_.history.seedExactDay(day, DailyReadingHistory::UNKNOWN_SECONDS);
      readingDaysMissingTime_ = true;
      return;
    }
    uint32_t current = 0;
    const uint32_t seconds = std::min<uint32_t>(saturatedSeconds(milliseconds), 24u * 3600u);
    if (output_.history.valueForDay(day, current)) {
      output_.history.seedExactDay(day, std::min<uint32_t>(24u * 3600u, addReadingStatsSaturated(current, seconds)));
    } else {
      output_.history.seedExactDay(day, seconds);
    }
    readingDaysTotalMs_ = std::min<uint64_t>(UINT64_MAX - readingDaysTotalMs_, milliseconds) + readingDaysTotalMs_;
  }

  void finishBook() {
    if (!book_.hasTotalReadingMs) ++booksMissingTime_;
    if (!book_.hasSessions) ++booksMissingSessions_;
    if (!book_.hasCompleted) ++booksMissingCompletion_;
    if (book_.hasTotalReadingMs) {
      bookTotalMs_ = std::min<uint64_t>(UINT64_MAX - bookTotalMs_, book_.totalReadingMs) + bookTotalMs_;
    }
    if (book_.hasSessions) {
      bookSessions_ = std::min<uint64_t>(UINT64_MAX - bookSessions_, book_.sessions) + bookSessions_;
    }
    if (book_.hasCompleted && book_.completed && completedBooks_ < UINT32_MAX) ++completedBooks_;

    if (!validBookPath(book_.path)) book_.path.clear();
    if (book_.path.empty() && validBookPath(book_.alternatePath)) book_.path = book_.alternatePath;
    if (!book_.path.empty()) {
      if (output_.summary.resolvableBooks < UINT16_MAX) ++output_.summary.resolvableBooks;
      if (visitor_ && !visitor_(visitorContext_, book_)) valid_ = false;
    }
    book_ = {};
  }

  void scalarTypeErrorIfRecognized() {
    if (context() == Context::Books || context() == Context::SessionLog ||
        (context() == Context::Root && key_ != Key::Unknown) ||
        (context() == Context::Book && key_ != Key::Unknown && key_ != Key::Path && key_ != Key::Title &&
         key_ != Key::Author && key_ != Key::TotalReadingMs && key_ != Key::Sessions && key_ != Key::FirstReadAt &&
         key_ != Key::CompletedAt && key_ != Key::Completed)) {
      valid_ = false;
    }
  }

  void onKey(const char* value, const size_t length) { key_ = parseKey(value, length); }

  void onString(const char* value, const size_t length) {
    if (context() == Context::Book) {
      if (key_ == Key::Path && length <= MAX_PATH_BYTES) book_.path.assign(value, length);
      if (key_ == Key::Title && length <= MAX_METADATA_BYTES) book_.title.assign(value, length);
      if (key_ == Key::Author && length <= MAX_METADATA_BYTES) book_.author.assign(value, length);
    } else if (context() == Context::KnownPaths && book_.alternatePath.empty() && length <= MAX_PATH_BYTES) {
      std::string candidate(value, length);
      if (validBookPath(candidate)) book_.alternatePath = std::move(candidate);
    } else {
      scalarTypeErrorIfRecognized();
    }
    key_ = Key::Unknown;
  }

  void onNumber(const char* value, const size_t length) {
    uint64_t number = 0;
    if (!parseUnsigned(value, length, number)) {
      valid_ = false;
      return;
    }
    switch (context()) {
      case Context::Root:
        if (key_ == Key::FormatVersion && number <= UINT32_MAX)
          formatVersion_ = static_cast<uint32_t>(number);
        else
          scalarTypeErrorIfRecognized();
        break;
      case Context::ReadingDays:
        if (number <= UINT32_MAX) addDay(static_cast<uint32_t>(number), 0, false);
        break;
      case Context::ReadingDay:
        if (key_ == Key::DayOrdinal && number <= UINT32_MAX) {
          dayOrdinal_ = static_cast<uint32_t>(number);
          dayHasOrdinal_ = true;
        } else if (key_ == Key::ReadingMs) {
          dayReadingMs_ = number;
          dayHasReadingMs_ = true;
        }
        break;
      case Context::Book:
        if (key_ == Key::TotalReadingMs) {
          book_.totalReadingMs = number;
          book_.hasTotalReadingMs = true;
        } else if (key_ == Key::Sessions) {
          book_.sessions = number;
          book_.hasSessions = true;
        } else if (key_ == Key::FirstReadAt) {
          book_.firstReadAt = number;
        } else if (key_ == Key::CompletedAt) {
          book_.completedAt = number;
        }
        break;
      default:
        scalarTypeErrorIfRecognized();
        break;
    }
    key_ = Key::Unknown;
  }

  void onBool(const bool value) {
    if (context() == Context::Book && key_ == Key::Completed) {
      book_.completed = value;
      book_.hasCompleted = true;
    } else {
      scalarTypeErrorIfRecognized();
    }
    key_ = Key::Unknown;
  }

  void onNull() {
    scalarTypeErrorIfRecognized();
    key_ = Key::Unknown;
  }

  void onObjectStart() {
    if (!rootSeen_ && depth_ == 0) {
      rootSeen_ = true;
      push(Context::Root);
    } else if (context() == Context::ReadingDays || context() == Context::BookReadingDays) {
      dayOrdinal_ = 0;
      dayReadingMs_ = 0;
      dayHasOrdinal_ = false;
      dayHasReadingMs_ = false;
      push(Context::ReadingDay);
    } else if (context() == Context::SessionLog) {
      push(Context::Session);
    } else if (context() == Context::Books) {
      book_ = {};
      push(Context::Book);
    } else {
      push(Context::IgnoreObject);
    }
    key_ = Key::Unknown;
  }

  void onObjectEnd() {
    if (depth_ == 0) {
      valid_ = false;
      return;
    }
    const Context ended = stack_[--depth_];
    if (ended == Context::ReadingDay && depth_ > 0 && stack_[depth_ - 1] == Context::ReadingDays) {
      if (dayHasOrdinal_) addDay(dayOrdinal_, dayReadingMs_, dayHasReadingMs_);
    } else if (ended == Context::Book) {
      finishBook();
    }
    key_ = Key::Unknown;
  }

  void onArrayStart() {
    Context next = Context::IgnoreArray;
    if (context() == Context::Root) {
      if (key_ == Key::ReadingDays) {
        next = Context::ReadingDays;
        sawReadingDays_ = true;
      } else if (key_ == Key::LegacyReadingDays) {
        next = Context::IgnoreArray;
        sawLegacyDays_ = true;
      } else if (key_ == Key::SessionLog) {
        next = Context::SessionLog;
        sawSessionLog_ = true;
      } else if (key_ == Key::Books) {
        next = Context::Books;
        sawBooks_ = true;
      }
    } else if (context() == Context::Book && key_ == Key::KnownPaths) {
      next = Context::KnownPaths;
    } else if (context() == Context::Book && key_ == Key::ReadingDays) {
      next = Context::BookReadingDays;
    }
    push(next);
    key_ = Key::Unknown;
  }

  void onArrayEnd() {
    if (depth_ == 0) {
      valid_ = false;
      return;
    }
    --depth_;
    key_ = Key::Unknown;
  }

  static void onKeyThunk(void* self, const char* value, size_t length) {
    static_cast<VCodexSaxHandler*>(self)->onKey(value, length);
  }
  static void onStringThunk(void* self, const char* value, size_t length) {
    static_cast<VCodexSaxHandler*>(self)->onString(value, length);
  }
  static void onNumberThunk(void* self, const char* value, size_t length) {
    static_cast<VCodexSaxHandler*>(self)->onNumber(value, length);
  }
  static void onBoolThunk(void* self, bool value) { static_cast<VCodexSaxHandler*>(self)->onBool(value); }
  static void onNullThunk(void* self) { static_cast<VCodexSaxHandler*>(self)->onNull(); }
  static void onObjectStartThunk(void* self) { static_cast<VCodexSaxHandler*>(self)->onObjectStart(); }
  static void onObjectEndThunk(void* self) { static_cast<VCodexSaxHandler*>(self)->onObjectEnd(); }
  static void onArrayStartThunk(void* self) { static_cast<VCodexSaxHandler*>(self)->onArrayStart(); }
  static void onArrayEndThunk(void* self) { static_cast<VCodexSaxHandler*>(self)->onArrayEnd(); }
};

bool parseSource(const char* sourcePath, ParsedDocument& output, BookVisitor visitor = nullptr,
                 void* visitorContext = nullptr) {
  output = {};
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, sourcePath, file)) return false;
  const size_t sourceSize = file.fileSize();
  if (sourceSize == 0 || sourceSize > MAX_SOURCE_BYTES) {
    file.close();
    return false;
  }
  output.sourceSize = static_cast<uint32_t>(sourceSize);
  VCodexSaxHandler handler(output, visitor, visitorContext);
  StreamingJsonParser parser(handler.callbacks());
  std::array<char, 512> buffer{};
  size_t remaining = sourceSize;
  while (remaining > 0) {
    const size_t wanted = std::min(remaining, buffer.size());
    const int read = file.read(buffer.data(), wanted);
    if (read != static_cast<int>(wanted)) {
      file.close();
      return false;
    }
    for (size_t index = 0; index < wanted; ++index) {
      output.sourceHash ^= static_cast<uint8_t>(buffer[index]);
      output.sourceHash *= 16777619u;
    }
    parser.feed(buffer.data(), wanted);
    if (parser.hasError()) {
      file.close();
      return false;
    }
    remaining -= wanted;
  }
  if (!file.close() || !parser.finish() || !handler.valid()) return false;
  handler.finalize();
  return true;
}

enum class SourceStatus : uint8_t { Missing, Invalid, Ready };

SourceStatus parseAvailableSource(ParsedDocument& output, const char*& sourcePath) {
  bool found = false;
  for (const char* candidate : {VCodexStatsImporter::SOURCE_PATH, VCodexStatsImporter::BACKUP_SOURCE_PATH}) {
    if (!Storage.exists(candidate)) continue;
    found = true;
    if (parseSource(candidate, output)) {
      sourcePath = candidate;
      return SourceStatus::Ready;
    }
  }
  return found ? SourceStatus::Invalid : SourceStatus::Missing;
}

bool globalStatsEmpty(const GlobalReadingStats& stats) {
  return stats.totalSessions == 0 && stats.totalReadingSeconds == 0 && stats.totalPagesTurned == 0 &&
         stats.completedBooks == 0 && stats.readingHistoryAnchorDay == 0 &&
         stats.readingHistoryBits == decltype(stats.readingHistoryBits){} && !stats.importedFromVCodex &&
         !stats.readingTimeUnavailable && !stats.sessionsUnavailable && !stats.pageTurnsUnavailable &&
         !stats.completionUnavailable;
}

bool historiesEqual(const DailyReadingHistory& left, const DailyReadingHistory& right) {
  if (left.hasAnchor() != right.hasAnchor() || left.anchorDay() != right.anchorDay() ||
      left.lifetimeReadingDays() != right.lifetimeReadingDays()) {
    return false;
  }
  if (!left.hasAnchor()) return true;
  for (size_t index = 0; index < DailyReadingHistory::DAY_COUNT; ++index) {
    if (index > left.anchorDay()) break;
    const uint32_t day = left.anchorDay() - static_cast<uint32_t>(index);
    uint32_t leftSeconds = 0;
    uint32_t rightSeconds = 0;
    const bool hasLeft = left.valueForDay(day, leftSeconds);
    const bool hasRight = right.valueForDay(day, rightSeconds);
    if (hasLeft != hasRight || (hasLeft && leftSeconds != rightSeconds)) return false;
  }
  return true;
}

bool crossViStatsEmpty(const bool allowImportedPartial = false) {
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats global = GlobalReadingStats::load(&globalStatus);
  if (!GlobalReadingStats::isTrustedLoadStatus(globalStatus) ||
      (!globalStatsEmpty(global) && !(allowImportedPartial && global.importedFromVCodex))) {
    return false;
  }

  DailyReadingHistory history;
  const DailyReadingHistory::LoadStatus historyStatus = DailyReadingHistory::load(history);
  if (historyStatus == DailyReadingHistory::LoadStatus::NewerVersion ||
      historyStatus == DailyReadingHistory::LoadStatus::IoError ||
      historyStatus == DailyReadingHistory::LoadStatus::Invalid || (!history.empty() && !allowImportedPartial)) {
    return false;
  }

  HalFile root = Storage.open("/.crosspoint");
  if (!root) return false;
  if (!root.isDirectory()) {
    root.close();
    return false;
  }
  char name[256]{};
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const bool directory = entry.isDirectory();
    const size_t length = entry.getName(name, sizeof(name));
    if (!entry.close() || length == 0 || length >= sizeof(name)) {
      root.close();
      return false;
    }
    if (!directory ||
        (strncmp(name, "epub_", 5) != 0 && strncmp(name, "txt_", 4) != 0 && strncmp(name, "xtc_", 4) != 0)) {
      continue;
    }
    const std::string cachePath = std::string("/.crosspoint/") + std::string(name, length);
    BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
    const BookReadingStats stats = BookReadingStats::load(cachePath, &status);
    if (!BookReadingStats::isTrustedLoadStatus(status) ||
        (stats.hasRecordedReading() && !(allowImportedPartial && stats.importedFromVCodex))) {
      root.close();
      return false;
    }
  }
  const bool iterationOk = root.getError() == 0;
  const bool closed = root.close();
  return iterationOk && closed;
}

struct ImportContext {
  int16_t utcOffsetMinutes = 0;
  bool ok = true;
};

bool importBook(void* opaque, const ParsedBook& source) {
  auto& context = *static_cast<ImportContext*>(opaque);
  const std::string cachePath = cachePathForBook(source.path);
  if (cachePath.empty() || (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))) {
    context.ok = false;
    return false;
  }
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
  const BookReadingStats existing = BookReadingStats::load(cachePath, &status);
  if (!BookReadingStats::isTrustedLoadStatus(status) ||
      (existing.hasRecordedReading() && !existing.importedFromVCodex)) {
    context.ok = false;
    return false;
  }

  BookReadingStats imported;
  imported.importedFromVCodex = true;
  imported.totalReadingSeconds = saturatedSeconds(source.totalReadingMs);
  imported.sessionCount = static_cast<uint16_t>(std::min<uint64_t>(source.sessions, UINT16_MAX));
  imported.isCompleted = source.completed;
  imported.readingTimeUnavailable = !source.hasTotalReadingMs;
  imported.sessionsUnavailable = !source.hasSessions;
  imported.pageTurnsUnavailable = true;
  imported.completionUnavailable = !source.hasCompleted;
  timestampToLocal(static_cast<uint32_t>(std::min<uint64_t>(source.firstReadAt, UINT32_MAX)), context.utcOffsetMinutes,
                   imported.startDate, imported.startMinuteOfDay);
  timestampToLocal(static_cast<uint32_t>(std::min<uint64_t>(source.completedAt, UINT32_MAX)), context.utcOffsetMinutes,
                   imported.finishedDate, imported.finishedMinuteOfDay);
  if (!imported.isCompleted) {
    imported.finishedDate.clear();
    imported.finishedMinuteOfDay = BookReadingStats::INVALID_MINUTE_OF_DAY;
  }
  if (!imported.saveRedundant(cachePath)) {
    context.ok = false;
    return false;
  }
  BookReadingStats::LoadStatus verifiedStatus = BookReadingStats::LoadStatus::Invalid;
  const BookReadingStats verified = BookReadingStats::load(cachePath, &verifiedStatus);
  context.ok = BookReadingStats::isTrustedLoadStatus(verifiedStatus) &&
               ReadingStatsCodec::encode(verified) == ReadingStatsCodec::encode(imported);
  return context.ok;
}

bool rollbackImportedBooks() {
  // Process one imported cache per pass so rollback has constant memory and
  // cannot silently stop after an arbitrary number of matched books.
  while (true) {
    HalFile root = Storage.open("/.crosspoint");
    if (!root) return true;
    if (!root.isDirectory()) {
      root.close();
      return false;
    }
    std::string importedCache;
    char name[256]{};
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      const bool directory = entry.isDirectory();
      const size_t length = entry.getName(name, sizeof(name));
      if (!entry.close()) {
        root.close();
        return false;
      }
      if (!directory || length == 0 || length >= sizeof(name) ||
          (strncmp(name, "epub_", 5) != 0 && strncmp(name, "txt_", 4) != 0 && strncmp(name, "xtc_", 4) != 0)) {
        continue;
      }
      const std::string cachePath = std::string("/.crosspoint/") + std::string(name, length);
      BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
      const BookReadingStats stats = BookReadingStats::load(cachePath, &status);
      if (!BookReadingStats::isTrustedLoadStatus(status)) {
        root.close();
        return false;
      }
      if (stats.importedFromVCodex) {
        importedCache = cachePath;
        break;
      }
    }
    const bool iterationOk = root.getError() == 0;
    if (!root.close() || !iterationOk) return false;
    if (importedCache.empty()) return true;
    if (!BookReadingStats::remove(importedCache)) return false;
  }
}

bool rollbackImport() {
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats global = GlobalReadingStats::load(&status);
  if (!GlobalReadingStats::isTrustedLoadStatus(status)) return false;
  if (global.importedFromVCodex && !GlobalReadingStats::resetLocal()) return false;
  if (!rollbackImportedBooks()) return false;
  if (!DailyReadingHistory::reset()) return false;
  return crossViStatsEmpty();
}

VCodexStatsImporter::ProbeResult probeInternal(VCodexStatsImportSummary& summary, const bool manual) {
  Marker marker;
  const MarkerStatus markerStatus = loadMarker(marker);
  if (markerStatus == MarkerStatus::IoError) return VCodexStatsImporter::ProbeResult::StorageError;
  if (!manual) {
    if (markerStatus == MarkerStatus::Invalid) return VCodexStatsImporter::ProbeResult::AlreadyAsked;
    if (markerStatus == MarkerStatus::Valid) {
      return marker.state == MarkerState::Pending ? VCodexStatsImporter::ProbeResult::PendingRecovery
                                                  : VCodexStatsImporter::ProbeResult::AlreadyAsked;
    }
  } else if (markerStatus == MarkerStatus::Valid && marker.state == MarkerState::Pending) {
    return VCodexStatsImporter::ProbeResult::PendingRecovery;
  }

  auto parsed = makeUniqueNoThrow<ParsedDocument>();
  if (!parsed) {
    LOG_ERR(LOG_TAG, "Not enough memory to inspect VCodex statistics");
    return VCodexStatsImporter::ProbeResult::StorageError;
  }
  const char* sourcePath = nullptr;
  const SourceStatus sourceStatus = parseAvailableSource(*parsed, sourcePath);
  if (sourceStatus == SourceStatus::Missing) return VCodexStatsImporter::ProbeResult::SourceMissing;
  if (sourceStatus != SourceStatus::Ready) return VCodexStatsImporter::ProbeResult::InvalidSource;
  if (!crossViStatsEmpty()) return VCodexStatsImporter::ProbeResult::CrossViNotEmpty;
  summary = parsed->summary;
  return VCodexStatsImporter::ProbeResult::Offer;
}

VCodexStatsImporter::ImportResult performImport(const int16_t utcOffsetMinutes, const bool pendingReplay) {
  auto document = makeUniqueNoThrow<ParsedDocument>();
  if (!document) {
    LOG_ERR(LOG_TAG, "Not enough memory to import VCodex statistics");
    return pendingReplay ? VCodexStatsImporter::ImportResult::RecoveryPending
                         : VCodexStatsImporter::ImportResult::Failed;
  }
  const char* sourcePath = nullptr;
  if (parseAvailableSource(*document, sourcePath) != SourceStatus::Ready) {
    if (pendingReplay && rollbackImport()) saveMarker(MarkerState::Failed, 0, 0);
    return VCodexStatsImporter::ImportResult::NotAvailable;
  }
  const uint32_t sourceSize = document->sourceSize;
  const uint32_t sourceHash = document->sourceHash;
  if (pendingReplay) {
    Marker marker;
    if (loadMarker(marker) != MarkerStatus::Valid || marker.state != MarkerState::Pending ||
        marker.sourceSize != sourceSize || marker.sourceHash != sourceHash) {
      if (rollbackImport()) saveMarker(MarkerState::Failed, sourceSize, sourceHash);
      return VCodexStatsImporter::ImportResult::Failed;
    }
  }
  if (!crossViStatsEmpty(pendingReplay)) return VCodexStatsImporter::ImportResult::NotEmpty;

  if (!pendingReplay && !saveMarker(MarkerState::Pending, sourceSize, sourceHash)) {
    return VCodexStatsImporter::ImportResult::Failed;
  }

  ImportContext context{utcOffsetMinutes, true};
  const bool replayValid = parseSource(sourcePath, *document, importBook, &context);
  const bool sourceUnchanged = replayValid && document->sourceSize == sourceSize && document->sourceHash == sourceHash;
  bool written = context.ok && sourceUnchanged;
  if (written) written = document->history.save();
  if (written) written = document->global.saveRedundant();

  if (written) {
    GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
    const GlobalReadingStats verifiedGlobal = GlobalReadingStats::load(&globalStatus);
    auto verifiedHistory = makeUniqueNoThrow<DailyReadingHistory>();
    if (!verifiedHistory) {
      written = false;
    } else {
      const DailyReadingHistory::LoadStatus historyStatus = DailyReadingHistory::load(*verifiedHistory);
      written = GlobalReadingStats::isTrustedLoadStatus(globalStatus) &&
                ReadingStatsCodec::encode(verifiedGlobal) == ReadingStatsCodec::encode(document->global) &&
                historyStatus != DailyReadingHistory::LoadStatus::Invalid &&
                historyStatus != DailyReadingHistory::LoadStatus::IoError &&
                historyStatus != DailyReadingHistory::LoadStatus::NewerVersion &&
                historiesEqual(*verifiedHistory, document->history);
    }
  }

  if (written && saveMarker(MarkerState::Completed, sourceSize, sourceHash)) {
    if (!ReadingAchievements::reconcileFromStorage()) {
      LOG_ERR(LOG_TAG, "Imported reading stats but could not reconcile achievements yet");
    }
    return VCodexStatsImporter::ImportResult::Imported;
  }

  if (!rollbackImport()) return VCodexStatsImporter::ImportResult::RecoveryPending;
  saveMarker(MarkerState::Failed, sourceSize, sourceHash);
  return VCodexStatsImporter::ImportResult::Failed;
}
}  // namespace

VCodexStatsImporter::ProbeResult VCodexStatsImporter::probe(VCodexStatsImportSummary& summary) {
  return probeInternal(summary, false);
}

VCodexStatsImporter::ProbeResult VCodexStatsImporter::probeManual(VCodexStatsImportSummary& summary) {
  return probeInternal(summary, true);
}

VCodexStatsImporter::ImportResult VCodexStatsImporter::import(const int16_t utcOffsetMinutes) {
  Marker marker;
  const MarkerStatus markerStatus = loadMarker(marker);
  if (markerStatus == MarkerStatus::IoError ||
      (markerStatus == MarkerStatus::Valid && marker.state == MarkerState::Pending)) {
    return ImportResult::NotAvailable;
  }
  return performImport(utcOffsetMinutes, false);
}

VCodexStatsImporter::ImportResult VCodexStatsImporter::decline() {
  auto inspected = makeUniqueNoThrow<ParsedDocument>();
  if (!inspected) return ImportResult::Failed;
  const char* sourcePath = nullptr;
  if (parseAvailableSource(*inspected, sourcePath) != SourceStatus::Ready) return ImportResult::NotAvailable;
  return saveMarker(MarkerState::Declined, inspected->sourceSize, inspected->sourceHash) ? ImportResult::Declined
                                                                                         : ImportResult::Failed;
}

VCodexStatsImporter::ImportResult VCodexStatsImporter::recoverPending(const int16_t utcOffsetMinutes) {
  Marker marker;
  if (loadMarker(marker) != MarkerStatus::Valid || marker.state != MarkerState::Pending) {
    return ImportResult::NotAvailable;
  }
  return performImport(utcOffsetMinutes, true);
}
