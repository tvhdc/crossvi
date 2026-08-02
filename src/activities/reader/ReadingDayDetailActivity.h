#pragma once

#include "ReadingCalendarModel.h"
#include "activities/Activity.h"

class ReadingDayDetailActivity final : public Activity {
 public:
  ReadingDayDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadingCalendarCell cell,
                           const ReadingCalendarSnapshot& snapshot)
      : Activity("ReadingDayDetail", renderer, mappedInput), cell_(cell), snapshot_(snapshot) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  ReadingCalendarCell cell_;
  ReadingCalendarSnapshot snapshot_;
  bool suppressInitialConfirmRelease_ = false;
};
