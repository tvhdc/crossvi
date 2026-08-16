#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "./FileBrowserActivity.h"
#include "HomeBookSummary.h"
#include "HomeMenuMapping.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;
class Epub;
class Xtc;
class Txt;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int carouselBookIndex = 0;
  bool firstRenderDone = false;
  bool mediaAvailable = false;
  bool completionStatsAlreadyRecovered = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool coverPreparationAttempted = false;
  bool bookSummaryPending = false;
  bool recentPrunePending = false;
  uint32_t coverPreparationLastInputAt = 0;
  size_t recentPruneIndex = 0;
  size_t pinnedPruneIndex = 0;
  int recentBookLimit = 0;
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
  std::unique_ptr<Epub> preparedEpub;
  std::unique_ptr<Xtc> preparedXtc;
  std::unique_ptr<Txt> preparedTxt;
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
  enum class SourcePreparationResult : uint8_t { NotNeeded, InProgress, Ready, Failed };
  SourcePreparationResult stepPreparedEpub(const std::string& path);
  SourcePreparationResult stepPreparedXtc(const std::string& path);
  SourcePreparationResult stepRecentNonEpubSummarySource();

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
  void processRecentBooksMaintenance();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE);
  ~HomeActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
};
