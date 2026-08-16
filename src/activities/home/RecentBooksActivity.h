#pragma once

#include <I18n.h>
#include <RawSourceIdentity.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryCatalogStore.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/OptionPopup.h"
#include "util/BookSearchUtils.h"
#include "util/ButtonNavigator.h"
#include "util/PressReleaseLatch.h"

class Epub;
class Xtc;

class RecentBooksActivity final : public Activity {
  enum class Tab : uint8_t { Recent = 0, All = 1 };

  struct AllSearchJob {
    BookSearchQuery query;
    std::vector<size_t> results;
    std::vector<LibraryBookRecord> batch;
    // The full sorted order remains in allSourceIndices.  Search only owns a
    // bounded result/batch buffer and walks the pin prefix before that shared
    // projection, avoiding a second up-to-8 KiB source-index vector.
    size_t pinnedCursor = 0;
    size_t sourceCursor = 0;
    size_t limit = 0;
    size_t exactCount = 0;
    uint32_t generation = 0;
    bool truncated = false;
    bool running = false;
  };

  OptionPopup optionPopup;
  Tab tab = Tab::Recent;
  size_t selectorIndex = 0;
  std::array<size_t, 2> rememberedBookIndex{};
  std::array<std::string, 2> rememberedBookPath;
  bool confirmPressSeen = false;
  bool confirmLongHandled = false;
  bool confirmTabHandled = false;
  bool confirmBookPressCaptured = false;
  bool pendingBookConfirmRelease = false;
  // Preserve a tab Confirm edge while the render task is busy decoding a
  // cached cover. It is applied as soon as the activity can safely take the
  // render lock.
  bool pendingTabConfirm = false;
  // Keep focus on the tab row while the all-books catalog is opened.  The
  // catalog may finish loading after the tab Confirm edge; restoring a book
  // immediately would make the first Down press land on a book instead of the
  // Search control.
  bool preserveTabFocus = false;
  bool suppressPopupConfirmRelease = false;
  int pendingPopupNavigation = 0;
  bool pendingPopupConfirmRelease = false;
  bool pendingPopupBackRelease = false;
  bool redrawBookActionsBackground = false;
  int pendingNavigation = 0;
  int pendingTabSwitch = 0;
  int pendingPageSwitch = 0;
  bool pendingSearch = false;
  bool pendingBack = false;
  ReaderUtils::HoldGestureState holdUp;
  ReaderUtils::HoldGestureState holdDown;
  ReaderUtils::HoldGestureState holdLeft;
  ReaderUtils::HoldGestureState holdRight;
  ReaderUtils::HoldGestureState holdBack;
  ButtonNavigator buttonNavigator_;
  ReleaseDebounceGuard navigationReleaseGuard;

  std::vector<RecentBook> recentBooks;
  std::array<bool, 2> searchActive{};
  std::array<bool, 2> searchResultsTruncated{};
  std::array<std::string, 2> searchQuery;
  std::array<std::vector<size_t>, 2> searchResults;
  AllSearchJob allSearchJob;
  uint32_t allSearchResultGeneration = 0;
  std::vector<size_t> pinnedSourceIndices;
  // When TXT hiding is enabled this is the visible projection of the raw
  // catalog. It stores only source indices, never metadata or cover pixels.
  std::vector<size_t> allSourceIndices;
  bool allSourceIndicesValid = false;
  uint8_t allSourceIndicesHideTxt = 0;
  uint8_t allSourceIndicesSort = CrossPointSettings::LIBRARY_SORT_DATE_ADDED_DESC;
  uint32_t pinnedProjectionGeneration = 0;
  bool pinnedProjectionValid = false;
  std::vector<LibraryBookRecord> renderPage;
  uint8_t* gridSnapshot = nullptr;
  size_t gridSnapshotSize = 0;
  Rect gridSnapshotRect;
  size_t gridSnapshotStart = 0;
  size_t gridSnapshotCount = 0;
  Tab gridSnapshotTab = Tab::Recent;
  uint8_t gridSnapshotGrid = 0;
  bool gridSnapshotValid = false;
  bool renderPageValid = false;
  size_t renderPageStart = 0;
  size_t renderPageCount = 0;
  Tab renderPageTab = Tab::Recent;
  bool allSearchPending = false;
  bool catalogOpenPending = false;
  StrId popupMessage = StrId::STR_NONE_OPT;
  unsigned long popupTime = 0;
  uint32_t lastCatalogCount = 0;
  LibraryCatalogStore::Phase lastCatalogPhase = LibraryCatalogStore::Phase::Idle;
  unsigned long lastCatalogRedrawMs = 0;
  bool storageAvailable = false;
  std::optional<YourBooksReturnState> pendingReturnState;
  bool restoreReturnAfterSearch = false;
  size_t coverQueuePageStart = static_cast<size_t>(-1);
  size_t coverQueueCursor = 0;
  size_t coverQueueSelected = 0;
  uint8_t coverQueueReadyMask = 0;
  uint8_t coverQueueShownMask = 0;
  uint32_t coverQueueLastInputAt = 0;
  std::unique_ptr<Epub> coverPreparationEpub;
  std::unique_ptr<Xtc> coverPreparationXtc;
  std::unique_ptr<Epub> preparedEpub;
  std::unique_ptr<Xtc> preparedXtc;
  std::unique_ptr<Txt> preparedTxt;
  std::string sourcePreparationFailedPath;
  std::string coverPreparationPath;
  std::optional<RawSourceIdentityHandoff> preparedEpubSourceIdentity;
  std::optional<RawSourceIdentityHandoff> preparedXtcSourceIdentity;

  size_t tabIndex() const { return static_cast<size_t>(tab); }
  bool allTab() const { return tab == Tab::All; }
  size_t controlCount() const { return 1; }
  bool tabSelected() const { return selectorIndex == 0; }
  bool bookSelected() const { return selectorIndex >= controlCount(); }
  size_t selectedBookIndex() const { return bookSelected() ? selectorIndex - controlCount() : 0; }
  size_t visibleBookCount() const;
  size_t sourceIndex(size_t visibleBookIndex) const;
  bool pinnedProjectionCurrent() const;
  bool loadVisibleBook(size_t visibleBookIndex, LibraryBookRecord& book) const;
  uint8_t viewMode() const;
  uint8_t gridMode() const;
  size_t pageCapacity() const;

  void loadRecentBooks();
  void selectTab(Tab next);
  void launchSearch();
  void applySearch(const std::string& query);
  void processAllSearchStep();
  void cancelAllSearch();
  void clearSearch(bool preserveQuery = false);
  void rebuildPinnedProjection();
  size_t allVisibleToSource(size_t visibleBookIndex) const;
  size_t allSourceToVisible(size_t sourceBookIndex) const;
  void showBookActions(size_t visibleBookIndex);
  void promptRemoveBook(const std::string& path, const std::string& title);
  bool refreshStorageAvailability();
  void queueNavigationInput();
  void applyPendingNavigation();
  void promptDeleteBook(size_t visibleIndex, const std::string& path, const std::string& title);
  void invalidateRenderPage();
  void invalidateGridSnapshot();
  bool restoreGridSnapshot(Rect rect, size_t pageStart, size_t pageCount, uint8_t gridSetting);
  bool storeGridSnapshot(Rect rect, size_t pageStart, size_t pageCount, uint8_t gridSetting);
  void freeGridSnapshot();
  void cancelCoverPreparation();
  void resetCoverQueue();
  bool coverCachesRequested() const;
  void processCoverQueue();
  void processSelectedSourcePreparation();
  void loadRenderPage(size_t pageStart, size_t count);
  void rememberCurrentBook();
  void restoreRememberedBook(bool locateByPath = false);
  void captureReaderReturnContext(const LibraryBookRecord& book) const;
  void openSelectedBook(const std::string& path);
  bool catalogLoading() const;
  int noticeHeight() const;
  Rect contentRect() const;

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::optional<YourBooksReturnState> returnState = std::nullopt);
  ~RecentBooksActivity() override;
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override {
    return !optionPopup.isActive() && handleSafeGlobalShortcut(shortcut);
  }
};
