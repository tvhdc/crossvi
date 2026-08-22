#include "ReadingAchievements.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>

#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr char LOG_TAG[] = "ACH";
constexpr char ACHIEVEMENTS_PATH[] = "/.crosspoint/achievements_v1.bin";
constexpr char ACHIEVEMENTS_BACKUP_PATH[] = "/.crosspoint/achievements_v1.bin.bak";
constexpr char ACHIEVEMENTS_TEMP_PATH[] = "/.crosspoint/achievements_v1.bin.tmp";
constexpr uint8_t PAYLOAD_VERSION = 2;
constexpr uint8_t LEGACY_PAYLOAD_VERSION = 1;
constexpr uint8_t FLAG_INITIALIZED = 1;
constexpr size_t LEGACY_PAYLOAD_SIZE = 2 + ReadingAchievementState::BYTE_COUNT;
constexpr size_t RECOGNITION_DAYS_OFFSET = LEGACY_PAYLOAD_SIZE;
constexpr size_t PAYLOAD_SIZE = RECOGNITION_DAYS_OFFSET + ReadingAchievements::COUNT * sizeof(uint32_t);
static_assert(PAYLOAD_SIZE <= ReadingStatsEnvelope::MAX_PAYLOAD_SIZE);

constexpr std::array<ReadingAchievementDefinition, ReadingAchievements::COUNT> DEFINITIONS = {{
    {0, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 1},
    {1, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 10},
    {2, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 25},
    {3, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 50},
    {4, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 100},
    {5, ReadingAchievementCategory::Sessions, ReadingAchievementMetric::Sessions, 200},
    {6, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 1u * 3600u},
    {7, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 5u * 3600u},
    {8, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 10u * 3600u},
    {9, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 24u * 3600u},
    {10, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 50u * 3600u},
    {11, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 100u * 3600u},
    {12, ReadingAchievementCategory::ReadingTime, ReadingAchievementMetric::ReadingSeconds, 200u * 3600u},
    {13, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 1},
    {14, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 2},
    {15, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 3},
    {16, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 5},
    {17, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 10},
    {18, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 25},
    {19, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 50},
    {20, ReadingAchievementCategory::CompletedBooks, ReadingAchievementMetric::CompletedBooks, 100},
    {21, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 10},
    {22, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 100},
    {23, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 500},
    {24, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 1000},
    {25, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 5000},
    {26, ReadingAchievementCategory::ForwardPages, ReadingAchievementMetric::ForwardPages, 10000},
    {27, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 3},
    {28, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 7},
    {29, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 14},
    {30, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 30},
    {31, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 60},
    {32, ReadingAchievementCategory::Streak, ReadingAchievementMetric::LongestStreak, 100},
    {33, ReadingAchievementCategory::ReadingDays, ReadingAchievementMetric::LifetimeReadingDays, 1},
    {34, ReadingAchievementCategory::ReadingDays, ReadingAchievementMetric::LifetimeReadingDays, 7},
    {35, ReadingAchievementCategory::ReadingDays, ReadingAchievementMetric::LifetimeReadingDays, 30},
    {36, ReadingAchievementCategory::ReadingDays, ReadingAchievementMetric::LifetimeReadingDays, 100},
    {37, ReadingAchievementCategory::ReadingDays, ReadingAchievementMetric::LifetimeReadingDays, 365},
}};

ReadingAchievementNotification pendingNotification;

enum class PathStatus : uint8_t { Missing, Valid, Invalid, NewerVersion, IoError };

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8u | static_cast<uint32_t>(data[2]) << 16u |
         static_cast<uint32_t>(data[3]) << 24u;
}

void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8u);
  data[2] = static_cast<uint8_t>(value >> 16u);
  data[3] = static_cast<uint8_t>(value >> 24u);
}

PathStatus readPath(const char* path, ReadingAchievementState* state = nullptr) {
  std::array<uint8_t, PAYLOAD_SIZE> payload{};
  const auto outcome =
      ReadingStatsEnvelope::read(path, ReadingStatsEnvelope::Kind::Achievements, payload.data(), payload.size());
  if (outcome.readResult == ReadingStatsStorage::ReadResult::Missing) return PathStatus::Missing;
  if (outcome.readResult != ReadingStatsStorage::ReadResult::Ok) return PathStatus::IoError;
  if (outcome.decodeResult == ReadingStatsEnvelope::DecodeResult::NewerFormat ||
      outcome.decodeResult == ReadingStatsEnvelope::DecodeResult::PayloadTooLarge) {
    return PathStatus::NewerVersion;
  }
  if (outcome.decodeResult == ReadingStatsEnvelope::DecodeResult::Ok && outcome.payloadSize >= 1 &&
      payload[0] > PAYLOAD_VERSION) {
    return PathStatus::NewerVersion;
  }
  if (outcome.decodeResult != ReadingStatsEnvelope::DecodeResult::Ok || outcome.payloadSize < 2 ||
      (payload[0] != LEGACY_PAYLOAD_VERSION && payload[0] != PAYLOAD_VERSION) ||
      (payload[0] == LEGACY_PAYLOAD_VERSION && outcome.payloadSize != LEGACY_PAYLOAD_SIZE) ||
      (payload[0] == PAYLOAD_VERSION && outcome.payloadSize != PAYLOAD_SIZE) || (payload[1] & ~FLAG_INITIALIZED) != 0 ||
      (payload[2 + ReadingAchievementState::BYTE_COUNT - 1] & 0xC0u) != 0) {
    return PathStatus::Invalid;
  }
  if (state) {
    state->initialized = (payload[1] & FLAG_INITIALIZED) != 0;
    std::copy_n(payload.begin() + 2, ReadingAchievementState::BYTE_COUNT, state->unlocked.begin());
    if (payload[0] == PAYLOAD_VERSION) {
      for (size_t id = 0; id < ReadingAchievements::COUNT; ++id) {
        state->unlockRecognitionDays[id] = readU32(payload.data() + RECOGNITION_DAYS_OFFSET + id * sizeof(uint32_t));
      }
    }
  }
  return PathStatus::Valid;
}

bool isProtected(const PathStatus status) {
  return status == PathStatus::NewerVersion || status == PathStatus::IoError;
}

bool saveState(const ReadingAchievementState& state) {
  const PathStatus primary = readPath(ACHIEVEMENTS_PATH);
  if (isProtected(primary) || isProtected(readPath(ACHIEVEMENTS_BACKUP_PATH)) ||
      isProtected(readPath(ACHIEVEMENTS_TEMP_PATH))) {
    return false;
  }
  std::array<uint8_t, PAYLOAD_SIZE> payload{};
  payload[0] = PAYLOAD_VERSION;
  payload[1] = state.initialized ? FLAG_INITIALIZED : 0;
  std::copy(state.unlocked.begin(), state.unlocked.end(), payload.begin() + 2);
  for (size_t id = 0; id < ReadingAchievements::COUNT; ++id) {
    writeU32(payload.data() + RECOGNITION_DAYS_OFFSET + id * sizeof(uint32_t), state.unlockRecognitionDays[id]);
  }
  return ReadingStatsEnvelope::writeAtomic(ACHIEVEMENTS_PATH, ACHIEVEMENTS_BACKUP_PATH, primary == PathStatus::Valid,
                                           ReadingStatsEnvelope::Kind::Achievements, payload.data(), payload.size());
}

ReadingAchievements::LoadStatus publicStatus(const PathStatus status) {
  switch (status) {
    case PathStatus::NewerVersion:
      return ReadingAchievements::LoadStatus::NewerVersion;
    case PathStatus::IoError:
      return ReadingAchievements::LoadStatus::IoError;
    case PathStatus::Invalid:
      return ReadingAchievements::LoadStatus::Invalid;
    case PathStatus::Missing:
    case PathStatus::Valid:
    default:
      return ReadingAchievements::LoadStatus::Missing;
  }
}
}  // namespace

bool ReadingAchievementState::isUnlocked(const uint8_t id) const {
  return id < ReadingAchievements::COUNT && (unlocked[id / 8u] & static_cast<uint8_t>(1u << (id % 8u))) != 0;
}

void ReadingAchievementState::unlock(const uint8_t id, const uint32_t recognitionDay) {
  if (id >= ReadingAchievements::COUNT || isUnlocked(id)) return;
  unlocked[id / 8u] |= static_cast<uint8_t>(1u << (id % 8u));
  unlockRecognitionDays[id] = recognitionDay;
}

uint32_t ReadingAchievementState::unlockRecognitionDay(const uint8_t id) const {
  return id < ReadingAchievements::COUNT ? unlockRecognitionDays[id] : 0;
}

uint8_t ReadingAchievementState::unlockedCount() const {
  uint8_t count = 0;
  for (uint8_t id = 0; id < ReadingAchievements::COUNT; ++id) count += isUnlocked(id) ? 1 : 0;
  return count;
}

const std::array<ReadingAchievementDefinition, ReadingAchievements::COUNT>& ReadingAchievements::definitions() {
  return DEFINITIONS;
}

ReadingAchievementSnapshot ReadingAchievements::snapshot(const GlobalReadingStats& stats,
                                                         const DailyReadingHistory& history) {
  ReadingAchievementSnapshot result;
  result.sessions = stats.sessionsUnavailable ? 0 : stats.totalSessions;
  result.readingSeconds = stats.readingTimeUnavailable ? 0 : stats.totalReadingSeconds;
  result.completedBooks = stats.completionUnavailable ? 0 : stats.completedBooks;
  result.forwardPages = stats.pageTurnsUnavailable ? 0 : stats.totalPagesTurned;
  result.longestStreak = stats.longestReadingStreak;
  result.lifetimeReadingDays = history.lifetimeReadingDays();
  return result;
}

uint32_t ReadingAchievements::progress(const ReadingAchievementDefinition& definition,
                                       const ReadingAchievementSnapshot& snapshot) {
  switch (definition.metric) {
    case ReadingAchievementMetric::Sessions:
      return snapshot.sessions;
    case ReadingAchievementMetric::ReadingSeconds:
      return snapshot.readingSeconds;
    case ReadingAchievementMetric::CompletedBooks:
      return snapshot.completedBooks;
    case ReadingAchievementMetric::ForwardPages:
      return snapshot.forwardPages;
    case ReadingAchievementMetric::LongestStreak:
      return snapshot.longestStreak;
    case ReadingAchievementMetric::LifetimeReadingDays:
      return snapshot.lifetimeReadingDays;
  }
  return 0;
}

ReadingAchievementEvaluation ReadingAchievements::evaluate(ReadingAchievementState& state,
                                                           const ReadingAchievementSnapshot& snapshot,
                                                           const uint32_t recognitionDay) {
  ReadingAchievementEvaluation result;
  for (const auto& definition : DEFINITIONS) {
    if (state.isUnlocked(definition.id) || progress(definition, snapshot) < definition.threshold) continue;
    state.unlock(definition.id, recognitionDay);
    if (result.firstUnlockedId == UINT8_MAX) result.firstUnlockedId = definition.id;
    ++result.newlyUnlocked;
  }
  return result;
}

ReadingAchievements::LoadStatus ReadingAchievements::load(ReadingAchievementState& state) {
  state = {};
  PathStatus status = readPath(ACHIEVEMENTS_PATH, &state);
  if (status == PathStatus::Valid) return LoadStatus::Ok;
  if (isProtected(status)) return publicStatus(status);
  bool invalid = status == PathStatus::Invalid;
  status = readPath(ACHIEVEMENTS_BACKUP_PATH, &state);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredBackup;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  status = readPath(ACHIEVEMENTS_TEMP_PATH, &state);
  if (status == PathStatus::Valid) return LoadStatus::RecoveredTemp;
  if (isProtected(status)) return publicStatus(status);
  invalid = invalid || status == PathStatus::Invalid;
  return invalid ? LoadStatus::Invalid : LoadStatus::Missing;
}

bool ReadingAchievements::reconcile(const GlobalReadingStats& stats, const DailyReadingHistory& history,
                                    ReadingAchievementEvaluation* evaluation) {
  ReadingAchievementState state;
  const LoadStatus status = load(state);
  if (status == LoadStatus::NewerVersion || status == LoadStatus::IoError) return false;

  const bool historical = !state.initialized;
  ReadingStatsDateTime now;
  const uint32_t recognitionDay = getCurrentLocalReadingStatsDateTime(now) ? readingStatsDayIndex(now.date) + 1u : 0u;
  // Historical backfill cannot reconstruct the actual threshold-crossing day,
  // so leave those dates unknown instead of presenting the migration day as an
  // unlock date. Live unlocks retain the current local day when available.
  const ReadingAchievementEvaluation unlocked =
      evaluate(state, snapshot(stats, history), historical ? 0u : recognitionDay);
  if (!state.initialized) state.initialized = true;
  if ((historical || unlocked.newlyUnlocked != 0) && !saveState(state)) return false;

  if (evaluation) *evaluation = unlocked;
  if (unlocked.newlyUnlocked != 0) {
    if (pendingNotification.count == 0) {
      pendingNotification = {unlocked.newlyUnlocked, unlocked.firstUnlockedId, historical};
    } else {
      pendingNotification.count = static_cast<uint8_t>(
          std::min<unsigned>(COUNT, static_cast<unsigned>(pendingNotification.count) + unlocked.newlyUnlocked));
      pendingNotification.historical = pendingNotification.historical || historical;
    }
  }
  return true;
}

bool ReadingAchievements::reconcileFromStorage(ReadingAchievementEvaluation* evaluation) {
  GlobalReadingStats::LoadStatus statsStatus = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats stats = GlobalReadingStats::load(&statsStatus);
  if (!GlobalReadingStats::isTrustedLoadStatus(statsStatus)) return false;
  DailyReadingHistory history;
  const DailyReadingHistory::LoadStatus historyStatus = DailyReadingHistory::load(history);
  if (historyStatus == DailyReadingHistory::LoadStatus::Invalid ||
      historyStatus == DailyReadingHistory::LoadStatus::NewerVersion ||
      historyStatus == DailyReadingHistory::LoadStatus::IoError) {
    return false;
  }
  return reconcile(stats, history, evaluation);
}

bool ReadingAchievements::reset() {
  pendingNotification = {};
  if (!Storage.exists(ACHIEVEMENTS_PATH) && !Storage.exists(ACHIEVEMENTS_BACKUP_PATH) &&
      !Storage.exists(ACHIEVEMENTS_TEMP_PATH)) {
    return true;
  }
  return saveState({});
}

bool ReadingAchievements::takePendingNotification(ReadingAchievementNotification& notification) {
  if (pendingNotification.count == 0) return false;
  notification = pendingNotification;
  pendingNotification = {};
  return true;
}
