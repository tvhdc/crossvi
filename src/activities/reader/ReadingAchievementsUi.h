#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdio>

#include "ReadingAchievements.h"

namespace ReadingAchievementsUi {

inline StrId titleKey(const uint8_t id) {
  static constexpr StrId TITLES[ReadingAchievements::COUNT] = {
      StrId::STR_ACHIEVEMENT_TITLE_FIRST_CHAPTER,
      StrId::STR_ACHIEVEMENT_TITLE_FINDING_RHYTHM,
      StrId::STR_ACHIEVEMENT_TITLE_STEADY_READER,
      StrId::STR_ACHIEVEMENT_TITLE_READING_HABIT,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_SESSIONS,
      StrId::STR_ACHIEVEMENT_TITLE_SEASONED_READER,
      StrId::STR_ACHIEVEMENT_TITLE_ONE_HOUR,
      StrId::STR_ACHIEVEMENT_TITLE_FIVE_HOURS,
      StrId::STR_ACHIEVEMENT_TITLE_TEN_HOURS,
      StrId::STR_ACHIEVEMENT_TITLE_ONE_DAY,
      StrId::STR_ACHIEVEMENT_TITLE_FIFTY_HOURS,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_HOURS,
      StrId::STR_ACHIEVEMENT_TITLE_TWO_HUNDRED_HOURS,
      StrId::STR_ACHIEVEMENT_TITLE_FIRST_FINISH,
      StrId::STR_ACHIEVEMENT_TITLE_SECOND_FINISH,
      StrId::STR_ACHIEVEMENT_TITLE_THREE_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_FIVE_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_TEN_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_TWENTY_FIVE_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_FIFTY_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_FINISHES,
      StrId::STR_ACHIEVEMENT_TITLE_TEN_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_FIVE_HUNDRED_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_THOUSAND_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_FIVE_THOUSAND_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_TEN_THOUSAND_TURNS,
      StrId::STR_ACHIEVEMENT_TITLE_THREE_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_SEVEN_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_FOURTEEN_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_THIRTY_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_SIXTY_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_DAY_STREAK,
      StrId::STR_ACHIEVEMENT_TITLE_ONE_READING_DAY,
      StrId::STR_ACHIEVEMENT_TITLE_SEVEN_READING_DAYS,
      StrId::STR_ACHIEVEMENT_TITLE_THIRTY_READING_DAYS,
      StrId::STR_ACHIEVEMENT_TITLE_HUNDRED_READING_DAYS,
      StrId::STR_ACHIEVEMENT_TITLE_YEAR_OF_READING,
  };
  return id < ReadingAchievements::COUNT ? TITLES[id] : TITLES[0];
}

inline void formatCondition(const ReadingAchievementDefinition& definition, char* buffer, const size_t size) {
  if (!buffer || size == 0) return;
  StrId format = StrId::STR_ACHIEVEMENT_CONDITION_SESSIONS;
  uint32_t value = definition.threshold;
  switch (definition.metric) {
    case ReadingAchievementMetric::Sessions:
      break;
    case ReadingAchievementMetric::ReadingSeconds:
      format = StrId::STR_ACHIEVEMENT_CONDITION_HOURS;
      value /= 3600u;
      break;
    case ReadingAchievementMetric::CompletedBooks:
      format = StrId::STR_ACHIEVEMENT_CONDITION_BOOKS;
      break;
    case ReadingAchievementMetric::ForwardPages:
      format = StrId::STR_ACHIEVEMENT_CONDITION_FORWARD_PAGES;
      break;
    case ReadingAchievementMetric::LongestStreak:
      format = StrId::STR_ACHIEVEMENT_CONDITION_STREAK;
      break;
    case ReadingAchievementMetric::LifetimeReadingDays:
      format = StrId::STR_ACHIEVEMENT_CONDITION_READING_DAYS;
      break;
  }
  snprintf(buffer, size, I18N.get(format), static_cast<unsigned long>(value));
}

inline void formatTitle(const ReadingAchievementDefinition& definition, char* buffer, const size_t size) {
  if (!buffer || size == 0) return;
  snprintf(buffer, size, "%s", I18N.get(titleKey(definition.id)));
}

}  // namespace ReadingAchievementsUi
