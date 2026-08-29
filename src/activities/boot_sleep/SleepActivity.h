#pragma once
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
  bool wakeFrameReplayable_ = true;

 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;
  bool wakeFrameReplayable() const { return wakeFrameReplayable_; }

 private:
  void renderDefaultSleepScreen();
  void renderCustomSleepScreen(bool withBookStats = false);
  void renderCoverSleepScreen(bool withBookStats = false);
  void renderReadingCalendarSleepScreen();
  void renderTransparentSleepScreen();
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool applyCoverFilter, bool withBookStats = false);
  void renderLastScreenSleepScreen();
  void renderBlankSleepScreen();
};
