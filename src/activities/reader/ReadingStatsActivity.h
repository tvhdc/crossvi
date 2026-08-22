#pragma once

#include <cstdint>
#include <string>

#include "ReadingStatsPresentation.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ReadingStatsActivity final : public Activity {
 public:
  enum class Page : uint8_t { Book, Device };

  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                       ReadingStatsPresentation presentation, Page initialPage, bool allowBookDateEdit = false,
                       bool allowDeviceBackup = false, std::string bookPath = {});

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override {
    return !optionPopup.isActive() && handleSafeGlobalShortcut(shortcut);
  }

 private:
  std::string bookTitle;
  std::string bookPath;
  ReadingStatsPresentation presentation;
  OptionPopup optionPopup;
  Page page;
  bool allowBookDateEdit;
  bool allowDeviceBackup;
  bool suppressInitialConfirmRelease = false;
};
