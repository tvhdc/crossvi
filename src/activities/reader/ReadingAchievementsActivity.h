#pragma once

#include "ReadingAchievements.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingAchievementsActivity final : public Activity {
 public:
  ReadingAchievementsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingAchievements", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  void moveSelection(int delta);

  ReadingAchievementState state_;
  ReadingAchievementSnapshot snapshot_;
  ButtonNavigator navigator_;
  uint8_t selectedId_ = 0;
  uint8_t unlockNoticeCount_ = 0;
  bool unlockNoticeHistorical_ = false;
  bool pendingNoticeAcknowledgement_ = false;
  bool available_ = false;
  bool showDetail_ = false;
  bool suppressInitialConfirmRelease_ = false;
};
