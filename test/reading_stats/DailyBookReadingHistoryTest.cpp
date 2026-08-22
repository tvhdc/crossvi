#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

#include "DailyBookReadingHistory.h"

namespace {
uint32_t dayIndex(const uint16_t year, const uint8_t month, const uint8_t day) {
  return readingStatsDayIndex({year, month, day});
}
}  // namespace

TEST(DailyBookReadingHistory, RecordsAndMergesBooksForOneDay) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 90));
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/two.txt", "Two", 30));
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/one.epub", "One renamed", 15));

  DailyBookReadingDay loaded;
  EXPECT_EQ(DailyBookReadingHistory::load(day, loaded), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(loaded.count, 2u);
  EXPECT_EQ(loaded.records[0].path, "/books/one.epub");
  EXPECT_EQ(loaded.records[0].title, "One renamed");
  EXPECT_EQ(loaded.records[0].seconds, 105u);
  EXPECT_EQ(loaded.records[1].path, "/books/two.txt");
  EXPECT_EQ(loaded.records[1].seconds, 30u);
}

TEST(DailyBookReadingHistory, RecordsEveryDayInASpanWithoutChangingTheGlobalHistoryFormat) {
  Storage.reset();
  DailyReadingHistoryDelta delta;
  delta.recordSpan({{2026, 8, 22}, 23, 59, 30}, 120);
  ASSERT_TRUE(DailyBookReadingHistory::record("/books/night.epub", "Night", delta));

  DailyBookReadingDay first;
  DailyBookReadingDay second;
  ASSERT_EQ(DailyBookReadingHistory::load(dayIndex(2026, 8, 22), first), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(DailyBookReadingHistory::load(dayIndex(2026, 8, 23), second), DailyBookReadingHistory::LoadStatus::Ok);
  ASSERT_EQ(first.count, 1u);
  ASSERT_EQ(second.count, 1u);
  EXPECT_EQ(first.records[0].seconds, 30u);
  EXPECT_EQ(second.records[0].seconds, 90u);
}

TEST(DailyBookReadingHistory, RecoversBackupAndRejectsNewerFiles) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10));
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20));

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
  EXPECT_FALSE(DailyBookReadingHistory::record(day, "/books/three.epub", "Three", 30));
  EXPECT_EQ(Storage.file(path), newer);
}

TEST(DailyBookReadingHistory, FailedPublicationKeepsThePreviousDay) {
  Storage.reset();
  const uint32_t day = dayIndex(2026, 8, 22);
  ASSERT_TRUE(DailyBookReadingHistory::record(day, "/books/one.epub", "One", 10));
  const std::string path = DailyBookReadingHistory::pathForDay(day);
  const auto previous = Storage.file(path);

  Storage.failRenameOnce();
  EXPECT_FALSE(DailyBookReadingHistory::record(day, "/books/two.epub", "Two", 20));
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
