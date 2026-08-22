#pragma once

#include <cstddef>

#include "DailyBookReadingHistory.h"
#include "ReadingCalendarModel.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingDayDetailActivity final : public Activity {
 public:
  ReadingDayDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadingCalendarCell cell)
      : Activity("ReadingDayDetail", renderer, mappedInput), cell_(cell) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  void move(int delta);

  ReadingCalendarCell cell_;
  DailyBookReadingDay books_;
  DailyBookReadingHistory::LoadStatus bookHistoryStatus_ = DailyBookReadingHistory::LoadStatus::Missing;
  ButtonNavigator navigator_;
  size_t selected_ = 0;
  bool historyRekeyPending_ = false;
  bool breakdownPartial_ = false;
  bool suppressInitialConfirmRelease_ = false;
};
