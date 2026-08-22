#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "LibraryCatalogStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FinishedBooksActivity final : public Activity {
 public:
  FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FinishedBooks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct CompletedBook {
    uint16_t catalogIndex = 0;
    uint16_t finishedDay = 0;
  };

  void stepCatalog();
  void finishCatalogScan();
  bool loadVisiblePage();
  const LibraryBookRecord* visibleRecord(size_t index) const;
  void move(int delta);
  void openSelectedStatistics();

  ButtonNavigator navigator_;
  std::vector<CompletedBook> books_;
  std::vector<LibraryBookRecord> visibleBooks_;
  size_t visiblePageStart_ = 0;
  uint32_t scanIndex_ = 0;
  uint32_t catalogGeneration_ = 0;
  size_t selected_ = 0;
  bool catalogScanStarted_ = false;
  bool catalogScanComplete_ = false;
  bool catalogScanPartial_ = false;
  bool statsLoadFailed_ = false;
  bool suppressInitialConfirmRelease_ = false;
};
