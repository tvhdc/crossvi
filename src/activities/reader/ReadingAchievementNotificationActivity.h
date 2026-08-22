#pragma once

#include <memory>

#include "ReadingAchievements.h"
#include "activities/Activity.h"

class ReadingAchievementNotificationActivity final : public Activity {
 public:
  ReadingAchievementNotificationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         ReadingAchievementNotification notification,
                                         std::unique_ptr<Activity> nextActivity);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void continueToDestination();

  ReadingAchievementNotification notification_;
  std::unique_ptr<Activity> nextActivity_;
  bool inputReleased_ = false;
};
