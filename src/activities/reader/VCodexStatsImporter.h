#pragma once

#include <cstdint>

struct VCodexStatsImportSummary {
  uint32_t totalReadingSeconds = 0;
  uint32_t totalSessions = 0;
  uint32_t completedBooks = 0;
  uint16_t calendarDays = 0;
  uint16_t resolvableBooks = 0;
  bool hasReadingTime = false;
  bool hasSessions = false;
  bool hasCompletedBooks = false;
  bool hasCalendarDays = false;
  bool hasResolvableBooks = false;
};

class VCodexStatsImporter {
 public:
  static constexpr const char* SOURCE_PATH = "/.crosspoint/reading_stats.json";

  enum class ProbeResult : uint8_t {
    Offer,
    SourceMissing,
    InvalidSource,
    AlreadyAsked,
    CrossViNotEmpty,
    PendingRecovery,
    StorageError,
  };

  enum class ImportResult : uint8_t { Imported, Declined, NotAvailable, NotEmpty, Failed, RecoveryPending };

  // Automatic startup probe. It never writes and never modifies the VCodex
  // source file.
  static ProbeResult probe(VCodexStatsImportSummary& summary);

  // Manual probe ignores a previous decline/failure but still refuses to
  // merge into non-empty CrossVi statistics.
  static ProbeResult probeManual(VCodexStatsImportSummary& summary);

  static ImportResult import(int16_t utcOffsetMinutes);
  static ImportResult decline();

  // Replays a power-loss-interrupted import. A Pending marker is never turned
  // into a second prompt.
  static ImportResult recoverPending(int16_t utcOffsetMinutes);
};
