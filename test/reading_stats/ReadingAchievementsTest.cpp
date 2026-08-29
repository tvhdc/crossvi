#include <HalStorage.h>
#include <gtest/gtest.h>

#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"
#include "ReadingAchievements.h"
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsUtils.h"

namespace {
bool clockValid = false;
ReadingStatsDateTime clockValue;
}  // namespace

bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime) {
  outDateTime = clockValid ? clockValue : ReadingStatsDateTime{};
  return clockValid;
}

namespace {
ReadingAchievementEvaluation evaluate(const ReadingAchievementSnapshot& snapshot, ReadingAchievementState& state) {
  return ReadingAchievements::evaluate(state, snapshot);
}
}  // namespace

TEST(ReadingAchievements, SessionThresholdsUnlockAtExactBoundaries) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 0);

  snapshot.sessions = 1;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 1);
  EXPECT_TRUE(state.isUnlocked(0));

  snapshot.sessions = 10;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 1);
  EXPECT_TRUE(state.isUnlocked(1));

  snapshot.sessions = 27;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 1);
  EXPECT_TRUE(state.isUnlocked(2));
  EXPECT_EQ(state.unlockedCount(), 3);
}

TEST(ReadingAchievements, UnlockRecognitionDayIsRecordedOnce) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  snapshot.sessions = 10;

  const auto first = ReadingAchievements::evaluate(state, snapshot, 1234);
  ASSERT_EQ(first.newlyUnlocked, 2);
  EXPECT_EQ(state.unlockRecognitionDay(0), 1234u);
  EXPECT_EQ(state.unlockRecognitionDay(1), 1234u);

  EXPECT_EQ(ReadingAchievements::evaluate(state, snapshot, 5678).newlyUnlocked, 0);
  EXPECT_EQ(state.unlockRecognitionDay(0), 1234u);
  EXPECT_EQ(state.unlockRecognitionDay(1), 1234u);
}

TEST(ReadingAchievements, RecognitionDayPersistsAndLegacyUnlocksRemainUndated) {
  Storage.reset();
  clockValid = true;
  clockValue.date = {2026, 8, 22};
  GlobalReadingStats stats;
  DailyReadingHistory history;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  ReadingAchievementNotification notification;
  EXPECT_FALSE(ReadingAchievements::peekPendingNotification(notification));

  stats.totalSessions = 1;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  ReadingAchievementState persisted;
  ASSERT_EQ(ReadingAchievements::load(persisted), ReadingAchievements::LoadStatus::Ok);
  EXPECT_EQ(persisted.unlockRecognitionDay(0), readingStatsDayIndex(clockValue.date) + 1u);
  clockValid = false;
  EXPECT_TRUE(ReadingAchievements::peekPendingNotification(notification));
  EXPECT_TRUE(ReadingAchievements::ackPendingNotification());

  const uint8_t legacyPayload[] = {1, 1, 1, 0, 0, 0, 0};
  ReadingStatsEnvelope::Bytes encoded{};
  const size_t encodedSize = ReadingStatsEnvelope::encode(ReadingStatsEnvelope::Kind::Achievements, legacyPayload,
                                                          sizeof(legacyPayload), encoded);
  ASSERT_NE(encodedSize, 0u);
  Storage.setFile("/.crosspoint/achievements_v1.bin",
                  std::vector<uint8_t>(encoded.begin(), encoded.begin() + encodedSize));
  ReadingAchievementState legacy;
  ASSERT_EQ(ReadingAchievements::load(legacy), ReadingAchievements::LoadStatus::Ok);
  EXPECT_TRUE(legacy.isUnlocked(0));
  EXPECT_EQ(legacy.unlockRecognitionDay(0), 0u);

  std::array<uint8_t, 159> datedPayload{};
  datedPayload[0] = 2;
  datedPayload[1] = 1;
  datedPayload[2] = 1;
  const size_t datedSize = ReadingStatsEnvelope::encode(ReadingStatsEnvelope::Kind::Achievements, datedPayload.data(),
                                                        datedPayload.size(), encoded);
  ASSERT_NE(datedSize, 0u);
  Storage.setFile("/.crosspoint/achievements_v1.bin",
                  std::vector<uint8_t>(encoded.begin(), encoded.begin() + datedSize));
  ReadingAchievementState dated;
  ASSERT_EQ(ReadingAchievements::load(dated), ReadingAchievements::LoadStatus::Ok);
  EXPECT_TRUE(dated.isUnlocked(0));
  EXPECT_FALSE(ReadingAchievements::peekPendingNotification(notification));
}

TEST(ReadingAchievements, RejectsAnnouncedAchievementThatIsStillLocked) {
  Storage.reset();
  constexpr size_t payloadSize =
      2 + 2 * ReadingAchievementState::BYTE_COUNT + ReadingAchievements::COUNT * sizeof(uint32_t);
  std::array<uint8_t, payloadSize> payload{};
  payload[0] = 3;
  payload[1] = 1;
  payload[2 + ReadingAchievementState::BYTE_COUNT] = 1;

  ReadingStatsEnvelope::Bytes encoded{};
  const size_t encodedSize =
      ReadingStatsEnvelope::encode(ReadingStatsEnvelope::Kind::Achievements, payload.data(), payload.size(), encoded);
  ASSERT_NE(encodedSize, 0u);
  Storage.setFile("/.crosspoint/achievements_v1.bin",
                  std::vector<uint8_t>(encoded.begin(), encoded.begin() + encodedSize));

  ReadingAchievementState state;
  EXPECT_EQ(ReadingAchievements::load(state), ReadingAchievements::LoadStatus::Invalid);
}

TEST(ReadingAchievements, AllMetricBoundariesUseCanonicalIntegerValues) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  snapshot.readingSeconds = 59u * 60u + 59u;
  snapshot.completedBooks = 2;
  snapshot.forwardPages = 99;
  snapshot.longestStreak = 6;
  snapshot.lifetimeReadingDays = 6;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 5);  // books 1+2, page 10, streak 3, day 1
  EXPECT_FALSE(state.isUnlocked(6));
  EXPECT_FALSE(state.isUnlocked(22));
  EXPECT_FALSE(state.isUnlocked(28));
  EXPECT_FALSE(state.isUnlocked(34));

  snapshot.readingSeconds = 3600;
  snapshot.forwardPages = 100;
  snapshot.longestStreak = 7;
  snapshot.lifetimeReadingDays = 7;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 4);  // 1h, page 100, streak 7, day 7
  EXPECT_TRUE(state.isUnlocked(6));
  EXPECT_TRUE(state.isUnlocked(22));
  EXPECT_TRUE(state.isUnlocked(28));
  EXPECT_TRUE(state.isUnlocked(34));
}

TEST(ReadingAchievements, JumpAcrossThresholdsUnlocksEachEligibleDefinitionOnce) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  snapshot.sessions = 120;
  const auto first = evaluate(snapshot, state);
  EXPECT_EQ(first.newlyUnlocked, 5);
  EXPECT_TRUE(state.isUnlocked(4));
  EXPECT_FALSE(state.isUnlocked(5));
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 0);
  EXPECT_EQ(state.unlockedCount(), 5);
}

TEST(ReadingAchievements, EveryCategoryUsesTheRequestedThresholds) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  snapshot.sessions = 200;
  snapshot.readingSeconds = 200u * 3600u;
  snapshot.completedBooks = 100;
  snapshot.forwardPages = 10000;
  snapshot.longestStreak = 100;
  snapshot.lifetimeReadingDays = 365;
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, ReadingAchievements::COUNT);
  EXPECT_EQ(state.unlockedCount(), ReadingAchievements::COUNT);
}

TEST(ReadingAchievements, RetroactiveReconcilePersistsOnlyUnlockStateAndDoesNotRewrite) {
  Storage.reset();
  GlobalReadingStats stats;
  stats.totalSessions = 27;
  stats.totalReadingSeconds = 24u * 3600u;
  DailyReadingHistory history;
  history.seedExactDay(readingStatsDayIndex({2026, 8, 20}), 60);

  ReadingAchievementEvaluation evaluation;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history, &evaluation));
  EXPECT_EQ(evaluation.newlyUnlocked, 3 + 4 + 1);
  ReadingAchievementNotification notification;
  ASSERT_TRUE(ReadingAchievements::peekPendingNotification(notification));
  EXPECT_TRUE(notification.historical);
  EXPECT_EQ(notification.count, evaluation.newlyUnlocked);

  ReadingAchievementState persisted;
  ASSERT_EQ(ReadingAchievements::load(persisted), ReadingAchievements::LoadStatus::Ok);
  EXPECT_TRUE(persisted.initialized);
  EXPECT_EQ(persisted.unlockRecognitionDay(0), 0u);
  const size_t writes = Storage.writeCallCount();
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history, &evaluation));
  EXPECT_EQ(evaluation.newlyUnlocked, 0);
  EXPECT_EQ(Storage.writeCallCount(), writes);
  ASSERT_TRUE(ReadingAchievements::ackPendingNotification());
  EXPECT_FALSE(ReadingAchievements::peekPendingNotification(notification));
}

TEST(ReadingAchievements, ReconcileReturnsThePersistedStateWithoutAnotherLoad) {
  Storage.reset();
  GlobalReadingStats stats;
  stats.totalSessions = 10;
  DailyReadingHistory history;

  ReadingAchievementEvaluation evaluation;
  ReadingAchievementState reconciled;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history, &evaluation, &reconciled));
  EXPECT_TRUE(reconciled.initialized);
  EXPECT_TRUE(reconciled.pendingHistoricalNotification);
  EXPECT_EQ(reconciled.unlockedCount(), 2);
  EXPECT_EQ(evaluation.newlyUnlocked, 2);

  ReadingAchievementState persisted;
  ASSERT_EQ(ReadingAchievements::load(persisted), ReadingAchievements::LoadStatus::Ok);
  EXPECT_EQ(reconciled.unlocked, persisted.unlocked);
  EXPECT_EQ(reconciled.announced, persisted.announced);
  EXPECT_EQ(reconciled.unlockRecognitionDays, persisted.unlockRecognitionDays);
}

TEST(ReadingAchievements, UnavailableImportedMetricsDoNotCreateFalseUnlocks) {
  GlobalReadingStats stats;
  stats.totalSessions = 999;
  stats.totalPagesTurned = 99999;
  stats.sessionsUnavailable = true;
  stats.pageTurnsUnavailable = true;
  DailyReadingHistory history;
  ReadingAchievementState state;
  const ReadingAchievementSnapshot snapshot = ReadingAchievements::snapshot(stats, history);
  EXPECT_FALSE(snapshot.isAvailable(ReadingAchievementMetric::Sessions));
  EXPECT_FALSE(snapshot.isAvailable(ReadingAchievementMetric::ForwardPages));
  EXPECT_TRUE(snapshot.isAvailable(ReadingAchievementMetric::ReadingSeconds));
  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 0);
}

TEST(ReadingAchievements, UnavailableMetricsAreSkippedEvenWhenProgressIsNonZero) {
  ReadingAchievementState state;
  ReadingAchievementSnapshot snapshot;
  snapshot.sessions = 200;
  snapshot.setAvailable(ReadingAchievementMetric::Sessions, false);

  EXPECT_EQ(evaluate(snapshot, state).newlyUnlocked, 0);
  EXPECT_FALSE(state.isUnlocked(0));
  EXPECT_FALSE(state.isUnlocked(5));
}

TEST(ReadingAchievements, ExplicitStatsResetClearsAchievementState) {
  Storage.reset();
  GlobalReadingStats stats;
  stats.totalSessions = 200;
  DailyReadingHistory history;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  ASSERT_TRUE(ReadingAchievements::reset());
  ReadingAchievementState state;
  ASSERT_EQ(ReadingAchievements::load(state), ReadingAchievements::LoadStatus::Ok);
  EXPECT_FALSE(state.initialized);
  EXPECT_EQ(state.unlockedCount(), 0);
}

TEST(ReadingAchievements, GlobalStatsResetClearsDerivedUnlockState) {
  Storage.reset();
  GlobalReadingStats stats;
  stats.totalSessions = 10;
  ASSERT_TRUE(stats.saveRedundant());
  DailyReadingHistory history;
  ASSERT_TRUE(history.save());
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  ReadingAchievementState state;
  ASSERT_EQ(ReadingAchievements::load(state), ReadingAchievements::LoadStatus::Ok);
  EXPECT_FALSE(state.initialized);
  EXPECT_EQ(state.unlockedCount(), 0);
}

TEST(ReadingAchievements, PendingNotificationsCoalesceAcrossCommits) {
  Storage.reset();
  GlobalReadingStats stats;
  DailyReadingHistory history;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  stats.totalSessions = 1;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  stats.completedBooks = 1;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  ReadingAchievementNotification notification;
  ASSERT_TRUE(ReadingAchievements::peekPendingNotification(notification));
  EXPECT_EQ(notification.count, 2);
  EXPECT_FALSE(notification.historical);
  EXPECT_EQ(notification.firstId, 0);
  EXPECT_TRUE(ReadingAchievements::peekPendingNotification(notification));
  ASSERT_TRUE(ReadingAchievements::ackPendingNotification());
  EXPECT_FALSE(ReadingAchievements::peekPendingNotification(notification));
}

TEST(ReadingAchievements, PendingNotificationPersistsUntilAcknowledged) {
  Storage.reset();
  GlobalReadingStats stats;
  DailyReadingHistory history;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  stats.totalSessions = 1;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  ReadingAchievementNotification first;
  ReadingAchievementNotification afterReload;
  ASSERT_TRUE(ReadingAchievements::peekPendingNotification(first));
  ASSERT_TRUE(ReadingAchievements::peekPendingNotification(afterReload));
  EXPECT_EQ(afterReload.count, first.count);
  EXPECT_EQ(afterReload.firstId, first.firstId);
  EXPECT_EQ(afterReload.historical, first.historical);

  ASSERT_TRUE(ReadingAchievements::ackPendingNotification());
  EXPECT_FALSE(ReadingAchievements::peekPendingNotification(afterReload));
}

TEST(ReadingAchievements, CorruptPrimaryRecoversThePreviousUnlockState) {
  Storage.reset();
  GlobalReadingStats stats;
  DailyReadingHistory history;
  stats.totalSessions = 10;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  stats.totalSessions = 25;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));
  auto corrupt = Storage.file("/.crosspoint/achievements_v1.bin");
  corrupt.back() ^= 0x80;
  Storage.setFile("/.crosspoint/achievements_v1.bin", corrupt);

  ReadingAchievementState recovered;
  ASSERT_EQ(ReadingAchievements::load(recovered), ReadingAchievements::LoadStatus::RecoveredBackup);
  EXPECT_TRUE(recovered.isUnlocked(0));
  EXPECT_TRUE(recovered.isUnlocked(1));
  EXPECT_FALSE(recovered.isUnlocked(2));
}

TEST(ReadingAchievements, NewerUnlockFileIsPreservedAndFailsClosed) {
  Storage.reset();
  GlobalReadingStats stats;
  DailyReadingHistory history;
  ASSERT_TRUE(ReadingAchievements::reconcile(stats, history));

  auto newer = Storage.file("/.crosspoint/achievements_v1.bin");
  ASSERT_GT(newer.size(), 12u);
  newer[8] = 4;
  const uint32_t crc = ReadingStatsEnvelope::crc32(newer.data(), newer.size() - sizeof(uint32_t));
  const size_t crcOffset = newer.size() - sizeof(uint32_t);
  newer[crcOffset] = static_cast<uint8_t>(crc);
  newer[crcOffset + 1] = static_cast<uint8_t>(crc >> 8);
  newer[crcOffset + 2] = static_cast<uint8_t>(crc >> 16);
  newer[crcOffset + 3] = static_cast<uint8_t>(crc >> 24);
  Storage.setFile("/.crosspoint/achievements_v1.bin", newer);

  ReadingAchievementState state;
  EXPECT_EQ(ReadingAchievements::load(state), ReadingAchievements::LoadStatus::NewerVersion);
  EXPECT_FALSE(ReadingAchievements::reset());
  EXPECT_EQ(Storage.file("/.crosspoint/achievements_v1.bin"), newer);
}
