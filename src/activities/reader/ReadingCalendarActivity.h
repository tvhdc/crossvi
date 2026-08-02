#pragma once

#include <utility>

#include "DailyReadingHistory.h"
#include "ReadingCalendarModel.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingCalendarActivity final : public Activity {
 public:
  ReadingCalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadingCalendarSnapshot snapshot)
      : Activity("ReadingCalendar", renderer, mappedInput), model_(std::move(snapshot)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  void moveDay(int delta);
  void moveMonth(int delta);
  void openSelectedDay();

  ReadingCalendarModel model_;
  DailyReadingHistory dailyHistory_;
  ButtonNavigator navigator_{700, 700};
  bool suppressInitialConfirmRelease_ = false;
};
