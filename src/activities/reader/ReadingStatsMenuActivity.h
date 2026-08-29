#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsMenuActivity final : public Activity {
 public:
  ReadingStatsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStatsMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  enum class Notice : uint8_t { None, Unavailable, BackupDone, BackupFailed, RestoreDone, RestoreFailed, NoBackup };

  void openSelected();
  void openOverview();
  void openCalendar();
  void handleStatsAction(const ActivityResult& result);
  void storeNotice(Notice notice);
  void setNotice(Notice notice);

  ButtonNavigator navigator_;
  int selectedIndex_ = 0;
  bool suppressInitialConfirmRelease_ = false;
  Notice notice_ = Notice::None;
};
