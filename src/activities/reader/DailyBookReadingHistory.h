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

  static std::string pathForDay(uint32_t day);
  static bool dayFromFileName(const char* name, uint32_t& day);
  static LoadStatus load(uint32_t day, DailyBookReadingDay& out);
  static bool record(uint32_t day, const std::string& path, const std::string& title, uint32_t seconds);
  static bool record(const std::string& path, const std::string& title, const DailyReadingHistoryDelta& delta);
  static bool reset();
};
