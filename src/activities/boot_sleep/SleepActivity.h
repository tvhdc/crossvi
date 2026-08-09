#pragma once
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingCalendarSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool applyCoverSettings) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
};
