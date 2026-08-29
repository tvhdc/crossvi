#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

#include "DailyBookReadingHistory.h"

namespace {
using RecordStatus = DailyBookReadingHistory::RecordStatus;

uint32_t dayIndex(const uint16_t year, const uint8_t month, const uint8_t day) {
  return readingStatsDayIndex({year, month, day});
}
}  // namespace

TEST(DailyBookReadingHistory, RecordsAndMergesBooksForOneDay) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 90), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/two.txt", "Two", 30), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One renamed", 15), RecordStatus::Ok);

  DailyBookReadingDay loaded;
  EXPECT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 2u);
  EXPECT_EQ(loaded.records[0].path, "/books/one.epub");
  EXPECT_EQ(loaded.records[0].title, "One renamed");
  EXPECT_EQ(loaded.records[0].seconds, 105u);
  EXPECT_EQ(loaded.records[1].path, "/books/two.txt");
  EXPECT_EQ(loaded.records[1].seconds, 30u);
}

TEST(DailyBookReadingHistory, ReusesThePrimaryValidationWhenUpdatingADay) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 25);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 90), RecordStatus::Ok);

  Storage.resetFaultInjection();
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 15), RecordStatus::Ok);

  // The primary is reused from load(). The two sibling reads protect newer or
  // unreadable artifacts; the final two reads verify staging and publication.
  EXPECT_EQ(Storage.openReadCallCount(DailyBookReadingHistory::pathForDay(day)), 2U);
  EXPECT_EQ(Storage.openReadCallCount(), 5U);
}

TEST(DailyBookReadingHistory, RecordsEveryDayInASpanWithoutChangingTheGlobalHistoryFormat) {
  Storage.reset();
  DailyReadingHistoryDelta delta;
  delta.recordSpan({{2026, 8, 22}, 23, 59, 30}, 120);
  ASSERT_EQ(DailyBookReadingHistory::record("/books/night.epub", "Night", delta), RecordStatus::Ok);

  DailyBookReadingDay first;
  DailyBookReadingDay second;
  ASSERT_EQ(DailyBookReadingHistory::load(dayIndex(2026, 8, 22), first), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::load(dayIndex(2026, 8, 23), second), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(first.count, 1u);
  ASSERT_EQ(second.count, 1u);
  EXPECT_EQ(first.records[0].seconds, 30u);
  EXPECT_EQ(second.records[0].seconds, 90u);
}

TEST(DailyBookReadingHistory, FullDayDoesNotDiscardLaterDaysInTheSameDelta) {
  Storage.reset();
  const uint32_t firstDay = dayIndex(2026, 8, 22);
  for (size_t index = 0; index < DailyBookReadingDay::MAX_BOOKS; ++index) {
    const std::string path = "/books/full-" + std::to_string(index) + ".epub";
    ASSERT_EQ(DailyBookReadingHistory::record(firstDay, path, "Full", 60), RecordStatus::Ok);
  }

  DailyReadingHistoryDelta delta;
  delta.recordSpan({{2026, 8, 22}, 23, 59, 30}, 120);
  EXPECT_EQ(DailyBookReadingHistory::record("/books/new.epub", "New", delta), RecordStatus::CapacityExceeded);

  DailyBookReadingDay first;
  DailyBookReadingDay second;
  ASSERT_EQ(DailyBookReadingHistory::load(firstDay, first), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::load(dayIndex(2026, 8, 23), second), DailyBookReadingHistory::LoadStatus::Ok);
  EXPECT_EQ(first.count, DailyBookReadingDay::MAX_BOOKS);
  ASSERT_EQ(second.count, 1u);
  EXPECT_EQ(second.records[0].path, "/books/new.epub");
  EXPECT_EQ(second.records[0].seconds, 90u);
}

TEST(DailyBookReadingHistory, RecoversBackupAndRejectsNewerFiles) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::Ok);

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  ASSERT_TRUE(Storage.exists((path + ".bak").c_str()));
  auto corrupt = Storage.file(path);
  corrupt.back() ^= 0x80;
  Storage.setFile(path, corrupt);
  DailyBookReadingDay recovered;
  EXPECT_EQ(DailyBookReadingHistory::load(day, recovered), DailyBookReadingHistory::LoadStatus::RecoveredBackup);
  ASSERT_EQ(recovered.count, 1u);
  EXPECT_EQ(recovered.records[0].path, "/books/one.epub");

  auto newer = Storage.file(path + ".bak");
  newer[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path, newer);
  Storage.setFile(path + ".bak", newer);
  DailyBookReadingDay protectedDay;
  EXPECT_EQ(DailyBookReadingHistory::load(day, protectedDay), DailyBookReadingHistory::LoadStatus::NewerVersion);
  EXPECT_EQ(DailyBookReadingHistory::record(day, "/books/three.epub", "Three", 30), RecordStatus::Protected);
  EXPECT_EQ(Storage.file(path), newer);
}

TEST(DailyBookReadingHistory, RecordDoesNotReplaceAnInvalidDayWithoutARecoveryCopy) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const std::vector<uint8_t> invalid = {0x00, 0x01, 0x02};
  Storage.setFile(path, invalid);

  EXPECT_EQ(DailyBookReadingHistory::record(day, "/books/new.epub", "New", 30), RecordStatus::IoError);
  EXPECT_EQ(Storage.file(path), invalid);
}

TEST(DailyBookReadingHistory, RecordStillUpdatesADayRecoveredFromBackup) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  Storage.setFile(path + ".bak", Storage.file(path));
  Storage.setFile(path, {0x00});

  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::Ok);
  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 2U);
  EXPECT_EQ(loaded.records[0].path, "/books/one.epub");
  EXPECT_EQ(loaded.records[1].path, "/books/two.epub");
}

TEST(DailyBookReadingHistory, ValidPrimaryDoesNotOverwriteNewerBackup) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::Ok);

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto primary = Storage.file(path);
  auto newerBackup = Storage.file(path + ".bak");
  ASSERT_FALSE(newerBackup.empty());
  newerBackup[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path + ".bak", newerBackup);

  EXPECT_EQ(DailyBookReadingHistory::record(day, "/books/three.epub", "Three", 30), RecordStatus::Protected);
  EXPECT_EQ(Storage.file(path), primary);
  EXPECT_EQ(Storage.file(path + ".bak"), newerBackup);
}

TEST(DailyBookReadingHistory, ValidPrimaryDoesNotOverwriteNewerTemp) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto primary = Storage.file(path);
  auto newerTemp = primary;
  newerTemp[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path + ".tmp", newerTemp);

  EXPECT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::Protected);
  EXPECT_EQ(Storage.file(path), primary);
  ASSERT_TRUE(Storage.exists((path + ".tmp").c_str()));
  EXPECT_EQ(Storage.file(path + ".tmp"), newerTemp);
}

TEST(DailyBookReadingHistory, ReadOnlyLoadStopsAfterAValidPrimary) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);

  Storage.resetFaultInjection();
  DailyBookReadingDay loaded;
  EXPECT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  EXPECT_EQ(Storage.openReadCallCount(), 1U);
}

TEST(DailyBookReadingHistory, ResetDoesNotDeleteNewerBackupAlongsideValidPrimary) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::Ok);

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto primary = Storage.file(path);
  auto newerBackup = Storage.file(path + ".bak");
  newerBackup[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path + ".bak", newerBackup);

  EXPECT_FALSE(DailyBookReadingHistory::canReset());
  EXPECT_FALSE(DailyBookReadingHistory::reset());
  EXPECT_EQ(Storage.file(path), primary);
  EXPECT_EQ(Storage.file(path + ".bak"), newerBackup);
}

TEST(DailyBookReadingHistory, ResetDoesNotDeleteNewerTempAlongsideValidPrimary) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto primary = Storage.file(path);
  auto newerTemp = primary;
  newerTemp[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path + ".tmp", newerTemp);

  EXPECT_FALSE(DailyBookReadingHistory::canReset());
  EXPECT_FALSE(DailyBookReadingHistory::reset());
  EXPECT_EQ(Storage.file(path), primary);
  EXPECT_EQ(Storage.file(path + ".tmp"), newerTemp);
}

TEST(DailyBookReadingHistory, FailedPublicationKeepsThePreviousDay) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10), RecordStatus::Ok);
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto previous = Storage.file(path);

  Storage.failRenameOnce();
  EXPECT_EQ(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20), RecordStatus::IoError);
  EXPECT_EQ(Storage.file(path), previous);
}

TEST(DailyBookReadingHistory, RecognizesOnlyCanonicalDayFileNames) {
  uint32_t day = 0;
  EXPECT_TRUE(DailyBookReadingHistory::dayFromFileName("9729.bin", day));
  EXPECT_EQ(day, 9729u);
  EXPECT_TRUE(DailyBookReadingHistory::dayFromFileName("0.bin", day));
  EXPECT_EQ(day, 0u);
  EXPECT_FALSE(DailyBookReadingHistory::dayFromFileName("9729.bin.bak", day));
  EXPECT_FALSE(DailyBookReadingHistory::dayFromFileName("09729.bin", day));
  EXPECT_FALSE(DailyBookReadingHistory::dayFromFileName("+1.bin", day));
  EXPECT_FALSE(DailyBookReadingHistory::dayFromFileName("1/2.bin", day));
  EXPECT_FALSE(DailyBookReadingHistory::dayFromFileName("42949672960.bin", day));
}

TEST(DailyBookReadingHistory, RekeysAndMergesMovedBooksWithoutDuplicatingTime) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 90), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/new.epub", "New", 30), RecordStatus::Ok);

  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));
  ASSERT_TRUE(DailyBookReadingHistory::finishPreparedRekey());
  ASSERT_TRUE(DailyBookReadingHistory::finishPreparedRekey());

  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 1u);
  EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
  EXPECT_EQ(loaded.records[0].title, "New");
  EXPECT_EQ(loaded.records[0].seconds, 120u);
}

TEST(DailyBookReadingHistory, RecoversPreparedMoveAccordingToAuthoritativeBookPath) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));
  Storage.setFile("/books/new.epub", {1});

  ASSERT_TRUE(DailyBookReadingHistory::recoverPreparedRekey());
  std::string alias;
  EXPECT_FALSE(DailyBookReadingHistory::pendingRekeyAlias("/books/new.epub", alias));
  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 1u);
  EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
  EXPECT_EQ(loaded.records[0].seconds, 60u);

  Storage.reset();
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));
  Storage.setFile("/books/old.epub", {1});
  ASSERT_TRUE(DailyBookReadingHistory::recoverPreparedRekey());
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  EXPECT_EQ(loaded.records[0].path, "/books/old.epub");
}

TEST(DailyBookReadingHistory, FailedRekeyPublicationRetainsAliasAndResumesIdempotently) {
  Storage.reset();
  const uint32_t firstDay = dayIndex(2026, 8, 22);
  const uint32_t secondDay = dayIndex(2026, 8, 23);
  ASSERT_EQ(DailyBookReadingHistory::record(firstDay, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::record(secondDay, "/books/old.epub", "Old", 120), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));

  Storage.failRenameOnce();
  EXPECT_FALSE(DailyBookReadingHistory::finishPreparedRekey());
  std::string alias;
  EXPECT_TRUE(DailyBookReadingHistory::pendingRekeyAlias("/books/new.epub", alias));
  EXPECT_EQ(alias, "/books/old.epub");

  Storage.setFile("/books/new.epub", {1});
  ASSERT_TRUE(DailyBookReadingHistory::recoverPreparedRekey());
  for (const uint32_t day : {firstDay, secondDay}) {
    DailyBookReadingDay loaded;
    ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
    ASSERT_EQ(loaded.count, 1u);
    EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
  }
}

TEST(DailyBookReadingHistory, RekeysADayRecoveredFromBackupOnly) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  Storage.setFile(path + ".bak", Storage.file(path));
  ASSERT_TRUE(Storage.remove(path.c_str()));

  ASSERT_TRUE(DailyBookReadingHistory::finishPreparedRekey());
  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 1U);
  EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
}

TEST(DailyBookReadingHistory, RekeysADayRecoveredFromTempOnly) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  Storage.setFile(path + ".tmp", Storage.file(path));
  ASSERT_TRUE(Storage.remove(path.c_str()));

  ASSERT_TRUE(DailyBookReadingHistory::finishPreparedRekey());
  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 1U);
  EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
}

TEST(DailyBookReadingHistory, RekeysEachDayOnlyOnceWhenSiblingsExist) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  Storage.setFile(path + ".bak", Storage.file(path));
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));

  Storage.resetFaultInjection();
  ASSERT_TRUE(DailyBookReadingHistory::finishPreparedRekey());
  // One marker read plus one bounded load/publish pass for this day.
  EXPECT_EQ(Storage.openReadCallCount(), 6U);

  DailyBookReadingDay loaded;
  ASSERT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 1U);
  EXPECT_EQ(loaded.records[0].path, "/books/new.epub");
}

TEST(DailyBookReadingHistory, NewerBackupOnlyKeepsPreparedRekeyForRecovery) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_EQ(DailyBookReadingHistory::record(day, "/books/old.epub", "Old", 60), RecordStatus::Ok);
  ASSERT_TRUE(DailyBookReadingHistory::prepareRekey("/books/old.epub", "/books/new.epub"));

  const std::string path = DailyBookReadingHistory::pathForDay(day);
  auto newerBackup = Storage.file(path);
  newerBackup[4] = DailyBookReadingHistory::VERSION + 1;
  Storage.setFile(path + ".bak", newerBackup);
  ASSERT_TRUE(Storage.remove(path.c_str()));

  EXPECT_FALSE(DailyBookReadingHistory::finishPreparedRekey());
  std::string alias;
  EXPECT_TRUE(DailyBookReadingHistory::pendingRekeyAlias("/books/new.epub", alias));
  EXPECT_EQ(alias, "/books/old.epub");
  EXPECT_EQ(Storage.file(path + ".bak"), newerBackup);
}
