#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/LibraryGridModel.h"

class BookStatsSelectionActivity final : public Activity {
 public:
  BookStatsSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookStatsSelection", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  static constexpr unsigned long LONG_PRESS_MS = 500;

  struct RenderState {
    size_t visibleCount = 0;
    size_t selectedIndex = 0;
    size_t pageSize = 1;
    bool statsLoadFailed = false;
  };

  void loadRecentBooks();
  void updateRenderState();
  size_t pageCapacity() const;
  void openSelectedBook();
  void rememberSelectedPath();
  size_t visibleCount() const;
  const RecentBook* selectedRecord() const;
  void moveSelection(int delta);
  void movePage(int delta);

  std::vector<RecentBook> recentBooks_;
  RenderState renderState_;
  std::string selectedPath_;
  size_t selectedIndex_ = 0;
  size_t pageSize_ = 1;
  ReaderUtils::HoldGestureState holdLeft_;
  ReaderUtils::HoldGestureState holdRight_;
  bool suppressInitialConfirmRelease_ = false;
};
