#include "ReadingAchievementsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "DailyReadingHistory.h"
#include "GlobalReadingStats.h"
#include "MappedInputManager.h"
#include "ReadingAchievementsUi.h"
#include "ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/icons/trophy.h"
#include "fontIds.h"

namespace {
constexpr std::array<uint8_t, 7> CATEGORY_STARTS = {0, 6, 13, 21, 27, 33, 38};
constexpr std::array<StrId, 6> CATEGORY_TITLES = {
    StrId::STR_ACHIEVEMENT_CATEGORY_SESSIONS, StrId::STR_ACHIEVEMENT_CATEGORY_TIME,
    StrId::STR_ACHIEVEMENT_CATEGORY_BOOKS,    StrId::STR_ACHIEVEMENT_CATEGORY_FORWARD_PAGES,
    StrId::STR_ACHIEVEMENT_CATEGORY_STREAK,   StrId::STR_ACHIEVEMENT_CATEGORY_READING_DAYS,
};

size_t categoryIndexFor(const uint8_t id) {
  for (size_t category = 0; category + 1 < CATEGORY_STARTS.size(); ++category) {
    if (id < CATEGORY_STARTS[category + 1]) return category;
  }
  return CATEGORY_TITLES.size() - 1;
}

std::string rowProgress(const ReadingAchievementDefinition& definition, const ReadingAchievementSnapshot& snapshot,
                        const bool unlocked) {
  if (unlocked) return I18N.get(StrId::STR_ACHIEVEMENT_UNLOCKED);
  if (!snapshot.isAvailable(definition.metric)) return I18N.get(StrId::STR_STATS_UNAVAILABLE);
  const uint32_t current = std::min(ReadingAchievements::progress(definition, snapshot), definition.threshold);
  char value[32];
  if (definition.metric == ReadingAchievementMetric::ReadingSeconds) {
    const uint32_t wholeHours = current / 3600u;
    const uint32_t minutes = current % 3600u / 60u;
    snprintf(value, sizeof(value), tr(STR_ACHIEVEMENT_PROGRESS_HOURS_FORMAT), static_cast<unsigned long>(wholeHours),
             static_cast<unsigned long>(minutes), static_cast<unsigned long>(definition.threshold / 3600u));
  } else {
    snprintf(value, sizeof(value), tr(STR_ACHIEVEMENT_PROGRESS_FORMAT), static_cast<unsigned long>(current),
             static_cast<unsigned long>(definition.threshold));
  }
  return value;
}
}  // namespace

void ReadingAchievementsActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!Storage.probeMedia()) {
    requestUpdate();
    return;
  }

  GlobalReadingStats::LoadStatus statsStatus = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats stats = GlobalReadingStats::load(&statsStatus);
  DailyReadingHistory history;
  const DailyReadingHistory::LoadStatus historyStatus = DailyReadingHistory::load(history);
  const bool historyTrusted = historyStatus == DailyReadingHistory::LoadStatus::Ok ||
                              historyStatus == DailyReadingHistory::LoadStatus::Missing ||
                              historyStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                              historyStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
  if (!GlobalReadingStats::isTrustedLoadStatus(statsStatus) || !historyTrusted ||
      !ReadingAchievements::reconcile(stats, history)) {
    requestUpdate();
    return;
  }
  const auto achievementStatus = ReadingAchievements::load(state_);
  if (achievementStatus == ReadingAchievements::LoadStatus::NewerVersion ||
      achievementStatus == ReadingAchievements::LoadStatus::IoError ||
      achievementStatus == ReadingAchievements::LoadStatus::Invalid) {
    requestUpdate();
    return;
  }
  snapshot_ = ReadingAchievements::snapshot(stats, history);
  ReadingAchievementNotification notification;
  if (ReadingAchievements::takePendingNotification(notification)) {
    unlockNoticeCount_ = notification.count;
    unlockNoticeHistorical_ = notification.historical;
  }
  available_ = true;
  requestUpdate();
}

void ReadingAchievementsActivity::moveSelection(const int delta) {
  int next = static_cast<int>(selectedId_) + delta;
  next = std::max(0, std::min(next, static_cast<int>(ReadingAchievements::COUNT) - 1));
  if (next == selectedId_) return;
  {
    RenderLock lock(*this);
    selectedId_ = static_cast<uint8_t>(next);
    unlockNoticeCount_ = 0;
  }
  requestUpdate();
}

void ReadingAchievementsActivity::loop() {
  if (suppressInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease_ = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (showDetail_) {
      {
        RenderLock lock(*this);
        showDetail_ = false;
      }
      requestUpdate();
      return;
    }
    finish();
    return;
  }
  if (showDetail_) return;
  navigator_.onNext([this] { moveSelection(1); });
  navigator_.onPrevious([this] { moveSelection(-1); });
  if (available_ && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock(*this);
      showDetail_ = true;
      unlockNoticeCount_ = 0;
    }
    requestUpdate();
  }
}

void ReadingAchievementsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_READING_ACHIEVEMENTS));

  const int contentX = safe.x + metrics.contentSidePadding;
  const int contentWidth = safe.width - metrics.contentSidePadding * 2;
  int y = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  if (!available_) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + metrics.headerHeight, tr(STR_STATS_UNAVAILABLE));
  } else if (showDetail_) {
    const auto& definition = ReadingAchievements::definitions()[selectedId_];
    const size_t category = categoryIndexFor(selectedId_);
    GUI.drawSubHeader(renderer, Rect{safe.x, y, safe.width, metrics.tabBarHeight}, I18N.get(CATEGORY_TITLES[category]),
                      tr(STR_ACHIEVEMENT_DETAILS));
    y += metrics.tabBarHeight + metrics.verticalSpacing * 2;

    renderer.drawIcon(TrophyIcon, safe.x + (safe.width - 32) / 2, y, 32);
    y += 32 + metrics.verticalSpacing;
    char title[96];
    ReadingAchievementsUi::formatTitle(definition, title, sizeof(title));
    renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;

    char condition[96];
    ReadingAchievementsUi::formatCondition(definition, condition, sizeof(condition));
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, condition, contentWidth, 3)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += metrics.verticalSpacing * 2;

    const bool progressAvailable = snapshot_.isAvailable(definition.metric);
    const uint32_t current = progressAvailable
                                 ? std::min(ReadingAchievements::progress(definition, snapshot_), definition.threshold)
                                 : 0;
    const int progressWidth = std::min(320, contentWidth);
    const int progressX = safe.x + (safe.width - progressWidth) / 2;
    renderer.drawRect(progressX, y, progressWidth, 16, true);
    const int progressFill =
        definition.threshold == 0
            ? 0
            : static_cast<int>(static_cast<uint64_t>(progressWidth - 4) * current / definition.threshold);
    if (progressFill > 0) renderer.fillRect(progressX + 2, y + 2, progressFill, 12, true);
    y += 16 + metrics.verticalSpacing;

    const std::string progress = rowProgress(definition, snapshot_, state_.isUnlocked(definition.id));
    char progressDetail[64];
    snprintf(progressDetail, sizeof(progressDetail), tr(STR_ACHIEVEMENT_PROGRESS_DETAIL_FORMAT), progress.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, y, progressDetail, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;

    if (!state_.isUnlocked(definition.id) && !progressAvailable) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_STATS_UNAVAILABLE));
    } else if (!state_.isUnlocked(definition.id)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_ACHIEVEMENT_IN_PROGRESS));
    } else {
      const uint32_t recognitionDay = state_.unlockRecognitionDay(definition.id);
      ReadingStatsDate recognizedDate;
      if (recognitionDay != 0 && readingStatsDateFromDayIndex(recognitionDay - 1u, recognizedDate)) {
        char unlockedOn[64];
        snprintf(unlockedOn, sizeof(unlockedOn), tr(STR_ACHIEVEMENT_UNLOCKED_ON_FORMAT),
                 static_cast<unsigned>(recognizedDate.day), static_cast<unsigned>(recognizedDate.month),
                 static_cast<unsigned>(recognizedDate.year));
        renderer.drawCenteredText(UI_10_FONT_ID, y, unlockedOn);
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_ACHIEVEMENT_UNLOCKED));
      }
    }
  } else {
    char summary[48];
    const uint8_t unlocked = state_.unlockedCount();
    snprintf(summary, sizeof(summary), tr(STR_ACHIEVEMENTS_SUMMARY_FORMAT), static_cast<unsigned>(unlocked),
             static_cast<unsigned>(ReadingAchievements::COUNT));
    renderer.drawText(UI_10_FONT_ID, contentX, y, summary, true, EpdFontFamily::BOLD);
    const int summaryWidth = renderer.getTextWidth(UI_10_FONT_ID, summary, EpdFontFamily::BOLD);
    const int barX = std::min(contentX + summaryWidth + 12, contentX + contentWidth - 80);
    const int barWidth = std::max(60, contentX + contentWidth - barX);
    renderer.drawRect(barX, y + 4, barWidth, 12, true);
    const int fill = (barWidth - 4) * unlocked / ReadingAchievements::COUNT;
    if (fill > 0) renderer.fillRect(barX + 2, y + 6, fill, 8, true);
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;

    if (unlockNoticeCount_ != 0) {
      char migrated[96];
      snprintf(migrated, sizeof(migrated),
               I18N.get(unlockNoticeHistorical_ ? StrId::STR_ACHIEVEMENTS_HISTORY_UNLOCKED_FORMAT
                                                : StrId::STR_ACHIEVEMENTS_RECOGNIZED_FORMAT),
               static_cast<unsigned>(unlockNoticeCount_));
      renderer.drawText(SMALL_FONT_ID, contentX, y, migrated, true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
    }

    const size_t category = categoryIndexFor(selectedId_);
    char page[32];
    snprintf(page, sizeof(page), tr(STR_ACHIEVEMENT_PAGE_FORMAT), static_cast<unsigned>(category + 1),
             static_cast<unsigned>(CATEGORY_TITLES.size()));
    GUI.drawSubHeader(renderer, Rect{safe.x, y, safe.width, metrics.tabBarHeight}, I18N.get(CATEGORY_TITLES[category]),
                      page);
    y += metrics.tabBarHeight + metrics.verticalSpacing;
    const int bottom = safe.y + safe.height - metrics.verticalSpacing;
    const uint8_t first = CATEGORY_STARTS[category];
    const uint8_t count = CATEGORY_STARTS[category + 1] - first;
    const int selected = selectedId_ - first;
    GUI.drawList(
        renderer, Rect{safe.x, y, safe.width, std::max(1, bottom - y)}, count, selected,
        [first](const int index) {
          char title[96];
          ReadingAchievementsUi::formatTitle(ReadingAchievements::definitions()[first + index], title, sizeof(title));
          return std::string(title);
        },
        [this, first](const int index) {
          const auto& definition = ReadingAchievements::definitions()[first + index];
          char condition[96];
          ReadingAchievementsUi::formatCondition(definition, condition, sizeof(condition));
          return std::string(condition);
        },
        nullptr,
        [this, first](const int index) {
          const auto& definition = ReadingAchievements::definitions()[first + index];
          return rowProgress(definition, snapshot_, state_.isUnlocked(definition.id));
        });
  }

  const auto labels = showDetail_ ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                                  : mappedInput.mapLabels(tr(STR_BACK), tr(STR_ACHIEVEMENT_DETAILS),
                                                          tr(STR_STATS_PREVIOUS), tr(STR_STATS_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
