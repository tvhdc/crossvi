#pragma once

#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "DailyReadingHistory.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BookReadingHistoryActivity final : public Activity {
 public:
  BookReadingHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             std::string bookTitle)
      : Activity("BookReadingHistory", renderer, mappedInput),
        bookPath_(std::move(bookPath)),
        bookTitle_(std::move(bookTitle)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return !scanComplete_; }
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  struct HistoryEntry {
    uint32_t day = 0;
    uint32_t seconds = 0;
  };

  void stepScan();
  void finishScan(bool failed);
  void addEntry(uint32_t day, uint32_t seconds);
  void move(int delta);

  std::string bookPath_;
  std::string bookTitle_;
  std::string pendingAlias_;
  HalFile directory_;
  std::array<HistoryEntry, READING_HISTORY_DAYS> entries_{};
  size_t entryCount_ = 0;
  size_t selected_ = 0;
  ButtonNavigator navigator_;
  bool scanComplete_ = false;
  bool scanPartial_ = false;
  bool suppressInitialConfirmRelease_ = false;
};
