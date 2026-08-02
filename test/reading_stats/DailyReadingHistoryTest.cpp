#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"

namespace {
constexpr char HISTORY_PATH[] = "/.crosspoint/daily_history_v1.bin";
constexpr char HISTORY_BACKUP_PATH[] = "/.crosspoint/daily_history_v1.bin.bak";
constexpr char HISTORY_TEMP_PATH[] = "/.crosspoint/daily_history_v1.bin.tmp";
constexpr char USER_HISTORY_BACKUP_PATH[] = "/.crosspoint/stats_backups/daily_history_v1.bin";

uint32_t dayIndex(const uint16_t year, const uint8_t month, const uint8_t day) {
  return readingStatsDayIndex({year, month, day});
}
}  // namespace

TEST(DailyReadingHistory, RoundTripsFixedSizeExactAndUnknownDays) {
  Storage.reset();
  DailyReadingHistory history;
  history.seedExactDay(dayIndex(2026, 7, 28), 75);
  history.seedExactDay(dayIndex(2026, 7, 27), 0);
  history.seedExactDay(dayIndex(2026, 7, 26), DailyReadingHistory::UNKNOWN_SECONDS);
  ASSERT_TRUE(history.save());
  ASSERT_TRUE(Storage.exists(HISTORY_PATH));
  EXPECT_EQ(Storage.file(HISTORY_PATH).size(), DailyReadingHistory::FILE_SIZE);

  DailyReadingHistory::LoadStatus status = DailyReadingHistory::LoadStatus::Invalid;
  DailyReadingHistory loaded;
  status = DailyReadingHistory::load(loaded);
  ASSERT_EQ(status, DailyReadingHistory::LoadStatus::Ok);
  uint32_t seconds = 0;
  ASSERT_TRUE(loaded.valueForDay(dayIndex(2026, 7, 28), seconds));
  EXPECT_EQ(seconds, 75u);
  ASSERT_TRUE(loaded.valueForDay(dayIndex(2026, 7, 27), seconds));
  EXPECT_EQ(seconds, 0u);
  EXPECT_FALSE(loaded.valueForDay(dayIndex(2026, 7, 26), seconds));
}

TEST(DailyReadingHistory, ReconcilesAValidButStaleLatestDaySummary) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 7, 28);
  DailyReadingHistory stale;
  stale.seedExactDay(day, 60);
  ASSERT_TRUE(stale.save());

  DailyReadingHistory loaded;
  ASSERT_EQ(DailyReadingHistory::load(loaded), DailyReadingHistory::LoadStatus::Ok);
  EXPECT_TRUE(loaded.reconcileExactDay(day, 180));
  EXPECT_FALSE(loaded.reconcileExactDay(day, 180));
  ASSERT_TRUE(loaded.save());

  DailyReadingHistory persisted;
  ASSERT_EQ(DailyReadingHistory::load(persisted), DailyReadingHistory::LoadStatus::Ok);
  uint32_t seconds = 0;
  ASSERT_TRUE(persisted.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 180u);
}

TEST(DailyReadingHistory, SplitsReadingAcrossMidnightWithoutDynamicGrowth) {
  DailyReadingHistoryDelta delta;
  delta.recordSpan({{2026, 7, 28}, 23, 59, 30}, 120);
  ASSERT_EQ(delta.count(), 2u);
  EXPECT_FALSE(delta.overflowed());

  DailyReadingHistory history;
  ASSERT_TRUE(history.apply(delta));
  uint32_t seconds = 0;
  ASSERT_TRUE(history.valueForDay(dayIndex(2026, 7, 28), seconds));
  EXPECT_EQ(seconds, 30u);
  ASSERT_TRUE(history.valueForDay(dayIndex(2026, 7, 29), seconds));
  EXPECT_EQ(seconds, 90u);
}

TEST(DailyReadingHistory, RejectsCrcCorruptionAndProtectsNewerVersion) {
  Storage.reset();
  DailyReadingHistory history;
  history.seedExactDay(dayIndex(2026, 7, 28), 60);
  ASSERT_TRUE(history.save());

  std::vector<uint8_t> corrupt = Storage.file(HISTORY_PATH);
  corrupt.back() ^= 0x80;
  Storage.setFile(HISTORY_PATH, corrupt);
  DailyReadingHistory::LoadStatus status = DailyReadingHistory::LoadStatus::Ok;
  DailyReadingHistory loaded;
  status = DailyReadingHistory::load(loaded);
  EXPECT_EQ(status, DailyReadingHistory::LoadStatus::Invalid);

  std::vector<uint8_t> newer = corrupt;
  newer[4] = 2;
  Storage.setFile(HISTORY_PATH, newer);
  status = DailyReadingHistory::load(loaded);
  EXPECT_EQ(status, DailyReadingHistory::LoadStatus::NewerVersion);
  EXPECT_FALSE(history.save());
  EXPECT_EQ(Storage.file(HISTORY_PATH), newer);
}

TEST(DailyReadingHistory, RecoversBackupThenTemporaryInOrder) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 7, 28);
  DailyReadingHistory first;
  first.seedExactDay(day, 10);
  ASSERT_TRUE(first.save());
  const std::vector<uint8_t> firstBytes = Storage.file(HISTORY_PATH);

  DailyReadingHistory second;
  second.seedExactDay(day, 20);
  ASSERT_TRUE(second.save());
  ASSERT_TRUE(Storage.exists(HISTORY_BACKUP_PATH));
  std::vector<uint8_t> corrupt = Storage.file(HISTORY_PATH);
  corrupt.back() ^= 1;
  Storage.setFile(HISTORY_PATH, corrupt);

  DailyReadingHistory::LoadStatus status = DailyReadingHistory::LoadStatus::Invalid;
  DailyReadingHistory loaded;
  status = DailyReadingHistory::load(loaded);
  ASSERT_EQ(status, DailyReadingHistory::LoadStatus::RecoveredBackup);
  uint32_t seconds = 0;
  ASSERT_TRUE(loaded.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 10u);

  Storage.setFile(HISTORY_BACKUP_PATH, {0x01});
  Storage.setFile(HISTORY_TEMP_PATH, firstBytes);
  status = DailyReadingHistory::load(loaded);
  ASSERT_EQ(status, DailyReadingHistory::LoadStatus::RecoveredTemp);
  ASSERT_TRUE(loaded.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 10u);
}

TEST(DailyReadingHistory, FailedAtomicReplacementKeepsPreviousHistory) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 7, 28);
  DailyReadingHistory first;
  first.seedExactDay(day, 30);
  ASSERT_TRUE(first.save());

  DailyReadingHistory replacement;
  replacement.seedExactDay(day, 90);
  Storage.shortWriteOnce();
  EXPECT_FALSE(replacement.save());

  DailyReadingHistory::LoadStatus status = DailyReadingHistory::LoadStatus::Invalid;
  DailyReadingHistory loaded;
  status = DailyReadingHistory::load(loaded);
  ASSERT_TRUE(status == DailyReadingHistory::LoadStatus::Ok ||
              status == DailyReadingHistory::LoadStatus::RecoveredBackup);
  uint32_t seconds = 0;
  ASSERT_TRUE(loaded.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 30u);
}

TEST(DailyReadingHistory, UserBackupRoundTripAndResetStayCrashSafe) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 7, 28);
  DailyReadingHistory original;
  original.seedExactDay(day, 120);
  ASSERT_TRUE(original.save());
  ASSERT_EQ(DailyReadingHistory::createBackup(), DailyReadingHistory::BackupResult::Ok);

  DailyReadingHistory changed;
  changed.seedExactDay(day, 360);
  ASSERT_TRUE(changed.save());
  ASSERT_EQ(DailyReadingHistory::restoreBackup(), DailyReadingHistory::BackupResult::Ok);

  DailyReadingHistory::LoadStatus status = DailyReadingHistory::LoadStatus::Invalid;
  DailyReadingHistory loaded;
  status = DailyReadingHistory::load(loaded);
  uint32_t seconds = 0;
  ASSERT_TRUE(loaded.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 120u);

  ASSERT_TRUE(DailyReadingHistory::reset());
  status = DailyReadingHistory::load(loaded);
  EXPECT_EQ(status, DailyReadingHistory::LoadStatus::Ok);
  EXPECT_FALSE(loaded.hasAnchor());
}

TEST(DailyReadingHistory, RejectsMoreDistinctPendingDaysThanItsBoundedDelta) {
  DailyReadingHistoryDelta delta;
  for (uint8_t offset = 0; offset <= DailyReadingHistoryDelta::MAX_DAYS; ++offset) {
    delta.recordSpan({{2026, 7, static_cast<uint8_t>(1 + offset)}, 12, 0, 0}, 60);
  }
  EXPECT_TRUE(delta.overflowed());
  DailyReadingHistory history;
  EXPECT_FALSE(history.apply(delta));
}

TEST(DailyReadingHistory, ALongSingleSpanStopsAtItsFixedDayCapacity) {
  DailyReadingHistoryDelta delta;
  delta.recordSpan({{2026, 1, 1}, 0, 0, 0}, UINT32_MAX);
  EXPECT_TRUE(delta.overflowed());
  EXPECT_EQ(delta.count(), DailyReadingHistoryDelta::MAX_DAYS);
}

TEST(DailyReadingHistory, GlobalSavePersistsBothSidesOfAMidnightSpan) {
  Storage.reset();
  GlobalReadingStats stats;
  stats.totalReadingSeconds = 120;
  stats.recordReadingSpan({{2026, 7, 28}, 23, 59, 30}, 120);
  ASSERT_TRUE(stats.save());

  DailyReadingHistory loaded;
  ASSERT_EQ(DailyReadingHistory::load(loaded), DailyReadingHistory::LoadStatus::Ok);
  uint32_t seconds = 0;
  ASSERT_TRUE(loaded.valueForDay(dayIndex(2026, 7, 28), seconds));
  EXPECT_EQ(seconds, 30u);
  ASSERT_TRUE(loaded.valueForDay(dayIndex(2026, 7, 29), seconds));
  EXPECT_EQ(seconds, 90u);
}

TEST(DailyReadingHistory, NewerSidecarIsPreservedWithoutBlockingCumulativeStats) {
  Storage.reset();
  DailyReadingHistory history;
  history.seedExactDay(dayIndex(2026, 7, 28), 60);
  ASSERT_TRUE(history.save());
  std::vector<uint8_t> newer = Storage.file(HISTORY_PATH);
  newer[4] = 2;
  Storage.setFile(HISTORY_PATH, newer);

  GlobalReadingStats stats;
  stats.totalReadingSeconds = 60;
  stats.recordReadingSpan({{2026, 7, 28}, 12, 0, 0}, 60);
  EXPECT_TRUE(stats.save());
  EXPECT_EQ(Storage.file(HISTORY_PATH), newer);

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
  EXPECT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  EXPECT_EQ(loaded.totalReadingSeconds, 60u);
}

TEST(DailyReadingHistory, GlobalBackupRestoresDailyHistoryAndResetClearsIt) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 7, 28);
  GlobalReadingStats stats;
  stats.totalReadingSeconds = 120;
  stats.recordReadingSpan({{2026, 7, 28}, 12, 0, 0}, 120);
  ASSERT_TRUE(stats.save());
  ASSERT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Ok);

  DailyReadingHistory changed;
  changed.seedExactDay(day, 300);
  ASSERT_TRUE(changed.save());
  ASSERT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::Ok);
  DailyReadingHistory restored;
  ASSERT_EQ(DailyReadingHistory::load(restored), DailyReadingHistory::LoadStatus::Ok);
  uint32_t seconds = 0;
  ASSERT_TRUE(restored.valueForDay(day, seconds));
  EXPECT_EQ(seconds, 120u);

  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  ASSERT_EQ(DailyReadingHistory::load(restored), DailyReadingHistory::LoadStatus::Ok);
  EXPECT_FALSE(restored.hasAnchor());
}

TEST(DailyReadingHistory, NewerDailyBackupBlocksRestoreBeforeGlobalStatsChange) {
  Storage.reset();
  GlobalReadingStats backedUp;
  backedUp.totalReadingSeconds = 60;
  backedUp.recordReadingSpan({{2026, 7, 28}, 12, 0, 0}, 60);
  ASSERT_TRUE(backedUp.save());
  ASSERT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Ok);

  std::vector<uint8_t> newer = Storage.file(USER_HISTORY_BACKUP_PATH);
  ASSERT_GT(newer.size(), 4u);
  newer[4] = 2;
  Storage.setFile(USER_HISTORY_BACKUP_PATH, newer);

  GlobalReadingStats current;
  current.totalReadingSeconds = 300;
  current.recordReadingSpan({{2026, 7, 29}, 12, 0, 0}, 300);
  ASSERT_TRUE(current.save());
  EXPECT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::NewerFormat);

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  EXPECT_EQ(loaded.totalReadingSeconds, 300u);
}
