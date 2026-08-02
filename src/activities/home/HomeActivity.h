#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "HomeBookSummary.h"
#include "HomeMenuMapping.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int carouselBookIndex = 0;
  bool firstRenderDone = false;
  bool mediaAvailable = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool coverPreparationAttempted = false;
  uint32_t coverPreparationLastInputAt = 0;
  // Home can be entered while Back is still held (e.g. leaving Settings with
  // Back): ignore that stale release until a fresh press is seen here.
  bool backPressSeen = false;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  HomeBookSummary bookSummary;
  const HomeMenuItem initialMenuItem;

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl, bool hasReadingStats) {
    return HomeMenuMapping::actionAt(idx, hasOpdsUrl, hasReadingStats);
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onYourBooksOpen();
  void onSavedItemsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReadingStatsOpen();
  bool hasReadingStatsShortcut() const;
  bool loadRecentNonEpubReadingStats();

  int getMenuItemCount() const;
  bool usesRecentListLayout() const;
  bool usesTripleCoverLayout() const;
  bool usesCarouselLayout() const;
  bool usesMultiBookCoverLayout() const;
  void selectHomeItem(int index);
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadBookSummary();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
};
