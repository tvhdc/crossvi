#include "ReadingAchievementNotificationActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "ReadingAchievementsUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

ReadingAchievementNotificationActivity::ReadingAchievementNotificationActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const ReadingAchievementNotification notification,
    std::unique_ptr<Activity> nextActivity)
    : Activity("ReadingAchievementNotification", renderer, mappedInput),
      notification_(notification),
      nextActivity_(std::move(nextActivity)) {}

void ReadingAchievementNotificationActivity::onEnter() {
  Activity::onEnter();
  inputReleased_ = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                   !mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingAchievementNotificationActivity::continueToDestination() {
  if (!ReadingAchievements::ackPendingNotification()) {
    LOG_ERR("ACH", "Failed to acknowledge achievement notification");
  }
  if (nextActivity_) activityManager.replaceActivity(std::move(nextActivity_));
}

void ReadingAchievementNotificationActivity::loop() {
  if (!inputReleased_) {
    inputReleased_ = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                     !mappedInput.isPressed(MappedInputManager::Button::Confirm);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    continueToDestination();
  }
}

void ReadingAchievementNotificationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_ACHIEVEMENTS_NEW));

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
  const int contentWidth = safe.width - metrics.contentSidePadding * 2;
  int y = contentTop;
  if (notification_.historical || notification_.count > 1) {
    char message[128];
    snprintf(message, sizeof(message),
             I18N.get(notification_.historical ? StrId::STR_ACHIEVEMENTS_HISTORY_UNLOCKED_FORMAT
                                               : StrId::STR_ACHIEVEMENTS_MULTI_UNLOCKED_FORMAT),
             static_cast<unsigned>(notification_.count));
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, message, contentWidth, 4)) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
  } else if (notification_.firstId < ReadingAchievements::COUNT) {
    const auto& definition = ReadingAchievements::definitions()[notification_.firstId];
    char title[96];
    ReadingAchievementsUi::formatTitle(definition, title, sizeof(title));
    renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;
    char condition[96];
    ReadingAchievementsUi::formatCondition(definition, condition, sizeof(condition));
    renderer.drawCenteredText(UI_10_FONT_ID, y, condition);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONTINUE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
