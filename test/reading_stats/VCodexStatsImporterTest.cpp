#include <HalStorage.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"
#include "ReadingAchievements.h"
#include "VCodexStatsImporter.h"

namespace {
constexpr char SOURCE_PATH[] = "/.crosspoint/reading_stats.json";
constexpr char BACKUP_SOURCE_PATH[] = "/.crosspoint/reading_stats.json.bak";
constexpr char BOOK_PATH[] = "/Books/Test.epub";

std::string bookCachePath() {
  return std::string("/.crosspoint/epub_") + std::to_string(std::hash<std::string>{}(BOOK_PATH));
}

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

std::string validSource() {
  return R"({
    "formatVersion": 6,
    "readingDays": [
      {"dayOrdinal": 20665, "readingMs": 3600000},
      {"dayOrdinal": 20666, "readingMs": 1800000}
    ],
    "legacyReadingDays": [],
    "sessionLog": [{"dayOrdinal": 20666, "sessionMs": 1800000, "path": "/Books/Test.epub"}],
    "books": [{
      "path": "/Books/Test.epub",
      "knownPaths": [],
      "title": "Test",
      "author": "Author",
      "totalReadingMs": 5400000,
      "sessions": 3,
      "firstReadAt": 1785542400,
      "completedAt": 1785628800,
      "completed": true,
      "readingDays": []
    }]
  })";
}

void seedSource(const std::string& json = validSource()) {
  Storage.reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/Books");
  Storage.setFile(SOURCE_PATH, bytes(json));
  Storage.setFile(BOOK_PATH, {1, 2, 3});
}
}  // namespace

TEST(VCodexStatsImporter, DetectsValidSourceOnlyWhenCrossViIsEmpty) {
  seedSource();
  VCodexStatsImportSummary summary;
  EXPECT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::Offer);
  EXPECT_EQ(summary.totalReadingSeconds, 5400u);
  EXPECT_EQ(summary.totalSessions, 3u);
  EXPECT_EQ(summary.completedBooks, 1u);
  EXPECT_EQ(summary.calendarDays, 2u);
  EXPECT_EQ(summary.resolvableBooks, 1u);
  EXPECT_TRUE(summary.hasCalendarDays);
  EXPECT_TRUE(summary.hasResolvableBooks);

  GlobalReadingStats existing;
  existing.totalReadingSeconds = 10;
  ASSERT_TRUE(existing.saveRedundant());
  EXPECT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::CrossViNotEmpty);
}

TEST(VCodexStatsImporter, DeclineIsRecordedOnceAndLeavesSourceUntouched) {
  seedSource();
  const std::vector<uint8_t> original = Storage.file(SOURCE_PATH);
  EXPECT_EQ(VCodexStatsImporter::decline(), VCodexStatsImporter::ImportResult::Declined);
  VCodexStatsImportSummary summary;
  EXPECT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::AlreadyAsked);
  EXPECT_EQ(Storage.file(SOURCE_PATH), original);
}

TEST(VCodexStatsImporter, RejectsMalformedSourceWithoutWritingMarker) {
  seedSource("{\"formatVersion\":6,\"books\":[");
  VCodexStatsImportSummary summary;
  EXPECT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::InvalidSource);
  EXPECT_FALSE(Storage.exists("/.crosspoint/vcodex_stats_import_v1.bin"));
}

TEST(VCodexStatsImporter, ImportsBackupWhenPrimaryIsMissingAndPreservesIt) {
  seedSource();
  const std::vector<uint8_t> backup = Storage.file(SOURCE_PATH);
  ASSERT_TRUE(Storage.remove(SOURCE_PATH));
  Storage.setFile(BACKUP_SOURCE_PATH, backup);

  VCodexStatsImportSummary summary;
  ASSERT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::Offer);
  EXPECT_EQ(summary.resolvableBooks, 1u);
  EXPECT_EQ(VCodexStatsImporter::import(0), VCodexStatsImporter::ImportResult::Imported);
  EXPECT_EQ(Storage.file(BACKUP_SOURCE_PATH), backup);
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST(VCodexStatsImporter, ImportsBackupWhenPrimaryIsInvalidAndPreservesBothSources) {
  seedSource();
  const std::vector<uint8_t> backup = Storage.file(SOURCE_PATH);
  const std::vector<uint8_t> invalidPrimary = bytes("{\"formatVersion\":6,\"books\":[");
  Storage.setFile(SOURCE_PATH, invalidPrimary);
  Storage.setFile(BACKUP_SOURCE_PATH, backup);

  VCodexStatsImportSummary summary;
  ASSERT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::Offer);
  EXPECT_EQ(VCodexStatsImporter::import(0), VCodexStatsImporter::ImportResult::Imported);
  EXPECT_EQ(Storage.file(SOURCE_PATH), invalidPrimary);
  EXPECT_EQ(Storage.file(BACKUP_SOURCE_PATH), backup);
}

TEST(VCodexStatsImporter, MissingSourceFieldsStayUnavailableInsteadOfBecomingZero) {
  seedSource(R"({"formatVersion":6,"sessionLog":[]})");
  VCodexStatsImportSummary summary;
  ASSERT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::Offer);
  EXPECT_FALSE(summary.hasReadingTime);
  EXPECT_FALSE(summary.hasSessions);
  EXPECT_FALSE(summary.hasCompletedBooks);
  EXPECT_FALSE(summary.hasCalendarDays);
  EXPECT_FALSE(summary.hasResolvableBooks);
  ASSERT_EQ(VCodexStatsImporter::import(0), VCodexStatsImporter::ImportResult::Imported);
  EXPECT_EQ(VCodexStatsImporter::import(0), VCodexStatsImporter::ImportResult::NotEmpty);
}

TEST(VCodexStatsImporter, ImportsOnceAndKeepsUnavailablePageMetricsExplicit) {
  seedSource();
  const std::vector<uint8_t> original = Storage.file(SOURCE_PATH);
  EXPECT_EQ(VCodexStatsImporter::import(7 * 60), VCodexStatsImporter::ImportResult::Imported);

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats global = GlobalReadingStats::load(&status);
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  EXPECT_EQ(global.totalReadingSeconds, 5400u);
  EXPECT_EQ(global.totalSessions, 3u);
  EXPECT_EQ(global.completedBooks, 1u);
  EXPECT_TRUE(global.importedFromVCodex);
  EXPECT_TRUE(global.pageTurnsUnavailable);

  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  const BookReadingStats book = BookReadingStats::load(bookCachePath(), &bookStatus);
  ASSERT_TRUE(BookReadingStats::isTrustedLoadStatus(bookStatus));
  EXPECT_EQ(book.totalReadingSeconds, 5400u);
  EXPECT_EQ(book.sessionCount, 3u);
  EXPECT_TRUE(book.isCompleted);
  EXPECT_TRUE(book.importedFromVCodex);
  EXPECT_TRUE(book.pageTurnsUnavailable);

  DailyReadingHistory history;
  ASSERT_EQ(DailyReadingHistory::load(history), DailyReadingHistory::LoadStatus::Ok);
  uint32_t seconds = 0;
  EXPECT_TRUE(history.valueForDay(20665u - 10957u, seconds));
  EXPECT_EQ(seconds, 3600u);
  EXPECT_TRUE(history.valueForDay(20666u - 10957u, seconds));
  EXPECT_EQ(seconds, 1800u);
  EXPECT_EQ(history.lifetimeReadingDays(), 2u);

  ReadingAchievementState achievements;
  ASSERT_EQ(ReadingAchievements::load(achievements), ReadingAchievements::LoadStatus::Ok);
  EXPECT_TRUE(achievements.isUnlocked(0));
  EXPECT_TRUE(achievements.isUnlocked(6));
  EXPECT_TRUE(achievements.isUnlocked(13));
  EXPECT_TRUE(achievements.isUnlocked(33));
  EXPECT_EQ(achievements.unlockedCount(), 4);

  VCodexStatsImportSummary summary;
  EXPECT_EQ(VCodexStatsImporter::probe(summary), VCodexStatsImporter::ProbeResult::AlreadyAsked);
  EXPECT_EQ(VCodexStatsImporter::import(7 * 60), VCodexStatsImporter::ImportResult::NotEmpty);
  EXPECT_EQ(Storage.file(SOURCE_PATH), original);
}

TEST(VCodexStatsImporter, FailedPublicationRollsBackAndAllowsManualRetry) {
  seedSource();
  Storage.failRenameOnCall(4);
  const auto result = VCodexStatsImporter::import(0);
  EXPECT_TRUE(result == VCodexStatsImporter::ImportResult::Failed ||
              result == VCodexStatsImporter::ImportResult::RecoveryPending);
  Storage.resetFaultInjection();

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats global = GlobalReadingStats::load(&status);
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  EXPECT_EQ(global.totalReadingSeconds, 0u);

  if (result == VCodexStatsImporter::ImportResult::RecoveryPending) {
    EXPECT_EQ(VCodexStatsImporter::recoverPending(0), VCodexStatsImporter::ImportResult::Imported);
  } else {
    VCodexStatsImportSummary summary;
    EXPECT_EQ(VCodexStatsImporter::probeManual(summary), VCodexStatsImporter::ProbeResult::Offer);
    EXPECT_EQ(VCodexStatsImporter::import(0), VCodexStatsImporter::ImportResult::Imported);
  }
}
