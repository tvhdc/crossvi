#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class DailyReadingHistory;
struct GlobalReadingStats;

constexpr size_t READING_ACHIEVEMENT_COUNT = 38;

enum class ReadingAchievementCategory : uint8_t {
  Sessions,
  ReadingTime,
  CompletedBooks,
  ForwardPages,
  Streak,
  ReadingDays,
};

enum class ReadingAchievementMetric : uint8_t {
  Sessions,
  ReadingSeconds,
  CompletedBooks,
  ForwardPages,
  LongestStreak,
  LifetimeReadingDays,
};

struct ReadingAchievementDefinition {
  uint8_t id;
  ReadingAchievementCategory category;
  ReadingAchievementMetric metric;
  uint32_t threshold;
};

struct ReadingAchievementSnapshot {
  uint32_t sessions = 0;
  uint32_t readingSeconds = 0;
  uint32_t completedBooks = 0;
  uint32_t forwardPages = 0;
  uint32_t longestStreak = 0;
  uint32_t lifetimeReadingDays = 0;
};

struct ReadingAchievementState {
  static constexpr size_t BYTE_COUNT = 5;

  std::array<uint8_t, BYTE_COUNT> unlocked{};
  // Day-index + 1 when CrossVi first recognized the unlock. Zero means that
  // older persisted data did not retain a date.
  std::array<uint32_t, READING_ACHIEVEMENT_COUNT> unlockRecognitionDays{};
  bool initialized = false;

  bool isUnlocked(uint8_t id) const;
  void unlock(uint8_t id, uint32_t recognitionDay = 0);
  uint32_t unlockRecognitionDay(uint8_t id) const;
  uint8_t unlockedCount() const;
};

struct ReadingAchievementEvaluation {
  uint8_t newlyUnlocked = 0;
  uint8_t firstUnlockedId = UINT8_MAX;
};

struct ReadingAchievementNotification {
  uint8_t count = 0;
  uint8_t firstId = UINT8_MAX;
  bool historical = false;
};

class ReadingAchievements {
 public:
  static constexpr size_t COUNT = READING_ACHIEVEMENT_COUNT;

  enum class LoadStatus : uint8_t { Ok, Missing, RecoveredBackup, RecoveredTemp, Invalid, NewerVersion, IoError };

  static const std::array<ReadingAchievementDefinition, COUNT>& definitions();
  static ReadingAchievementSnapshot snapshot(const GlobalReadingStats& stats, const DailyReadingHistory& history);
  static uint32_t progress(const ReadingAchievementDefinition& definition, const ReadingAchievementSnapshot& snapshot);
  static ReadingAchievementEvaluation evaluate(ReadingAchievementState& state,
                                               const ReadingAchievementSnapshot& snapshot, uint32_t recognitionDay = 0);

  static LoadStatus load(ReadingAchievementState& state);
  static bool reconcile(const GlobalReadingStats& stats, const DailyReadingHistory& history,
                        ReadingAchievementEvaluation* evaluation = nullptr);
  static bool reconcileFromStorage(ReadingAchievementEvaluation* evaluation = nullptr);
  static bool reset();

  static bool takePendingNotification(ReadingAchievementNotification& notification);
};
