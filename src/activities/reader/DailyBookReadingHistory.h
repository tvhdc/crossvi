#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "DailyReadingHistory.h"

struct DailyBookReadingRecord {
  std::string path;
  std::string title;
  uint32_t seconds = 0;
};

struct DailyBookReadingDay {
  static constexpr size_t MAX_BOOKS = 32;

  std::array<DailyBookReadingRecord, MAX_BOOKS> records{};
  size_t count = 0;
};

// Supplemental per-book breakdown for a calendar day. Each day is stored in
// its own bounded, CRC-protected file so opening one day never scans the whole
// library or allocates an unbounded history database.
class DailyBookReadingHistory {
 public:
  static constexpr char DIRECTORY[] = "/.crosspoint/daily_books_v1";
  static constexpr uint8_t VERSION = 1;
  static constexpr size_t MAX_PATH_BYTES = 511;
  static constexpr size_t MAX_TITLE_BYTES = 255;

  enum class LoadStatus : uint8_t {
    Ok,
    Missing,
    RecoveredBackup,
    RecoveredTemp,
    Invalid,
    NewerVersion,
    IoError,
  };

  enum class RecordStatus : uint8_t { Ok, CapacityExceeded, Protected, IoError };

  static std::string pathForDay(uint32_t day);
  static bool dayFromFileName(const char* name, uint32_t& day);
  static LoadStatus load(uint32_t day, DailyBookReadingDay& out);
  static RecordStatus record(uint32_t day, const std::string& path, const std::string& title, uint32_t seconds);
  // Attempts every independent day in the bounded delta. This prevents one
  // full day from discarding the per-book breakdown for later days.
  static RecordStatus record(const std::string& path, const std::string& title,
                             const DailyReadingHistoryDelta& delta);
  // Persists the old/new path before a book move. Finishing is idempotent and
  // rewrites each bounded day file atomically, so power-loss recovery can
  // safely continue without adding reading time twice.
  static bool prepareRekey(const std::string& oldPath, const std::string& newPath);
  static bool finishPreparedRekey();
  static bool cancelPreparedRekey(const std::string& oldPath, const std::string& newPath);
  static bool recoverPreparedRekey();
  // Returns the other path while a partially completed rekey remains. Readers
  // of per-book history can then match both identities until recovery finishes.
  static bool pendingRekeyAlias(const std::string& path, std::string& alias);
  static bool canReset();
  static bool reset();
};
