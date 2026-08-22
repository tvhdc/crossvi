/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossVi
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "GlobalReadingStats.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  std::atomic<int8_t> pendingPageTurnDelta{0};
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  std::atomic<uint32_t> debugTurnSequence{0};
#endif
  std::optional<uint32_t> initialBookmarkPage;
  uint32_t lastSavedPage = static_cast<uint32_t>(-1);
  ProgressFile::WriteSession progressWriteSession;
  std::atomic<uint32_t> lastSuccessfullyRenderedPage{std::numeric_limits<uint32_t>::max()};
  int pagesUntilFullRefresh = 0;
  ReaderUtils::X3ReaderWaveformState readerWaveform;
  bool deferredCoverRequested = false;
  bool deferredCoverFinished = false;
  uint32_t deferredCoverLastInputAt = 0;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  uint32_t deferredCoverStartedMs = 0;
#endif
  bool readerOpenStagesPending = true;
  std::unique_ptr<uint8_t[]> xtchPlaneScratch;
  size_t xtchPlaneScratchCapacity = 0;
  bool skipStartupRecentUpdate = false;
  bool deferredOpenStatePending = true;
  bool deferredOpenStateReady = false;
  bool readerStateSaveRetryPending = false;
  uint32_t deferredGlobalPageTurns = 0;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  BookReadingStats bookReadingStats;
  GlobalReadingStats globalReadingStats;
  bool completionStatsWritableAtOpen = true;
  bool bookReadingStatsTrusted = true;
  bool globalReadingStatsTrusted = true;
  bool bookReadingStatsWritable = true;
  bool globalReadingStatsWritable = true;
  bool completionAttemptBlocked = false;
  std::atomic<bool> pendingStatsCompletionError{false};
  ReadingSessionTracker readingSessionTracker;
  uint32_t sessionReadingSeconds = 0;
  BookReadingStats pendingBookReadingSpans;
  GlobalReadingStats pendingGlobalReadingSpans;
  ReadingStatsDateTime activeReadingSpanStartLocalDateTime;
  bool hasActiveReadingSpanStartLocalDateTime = false;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  bool readingSessionCommitted = false;
  bool bookReadingStatsDirty = false;
  bool globalReadingStatsDirty = false;
  bool dailyBookHistoryPending = false;
  std::atomic<int8_t> pendingReadingViewSignal{0};
  std::atomic<uint32_t> pendingReadingViewAtMs{0};
  bool ignoreNextConfirmRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  std::atomic<bool> pendingShortcutUnsupportedNotice{false};
  uint8_t autoPageTurnSeconds = 0;
  bool automaticPageTurnActive = false;
  std::atomic<unsigned long> lastPageTurnTime{0};
  std::atomic<bool> pendingScreenshot{false};
  std::vector<BookmarkEntry> cachedBookmarks;
  bool bookmarksLoaded = false;
  bool bookmarksWritable = false;
  bool currentPageBookmarked = false;
  std::atomic<bool> showBookmarkMessage{false};
  std::atomic<bool> bookmarkRemoved{false};
  std::atomic<bool> pendingBookmarkStorageError{false};
  unsigned long bookmarkMessageTime = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  enum class PageRenderResult : uint8_t { Displayed, Superseded, Error };
  bool retargetQueuedPageTurns();
  PageRenderResult renderPage(const std::shared_ptr<Xtc>& book, uint32_t page);
  // Opens chapter selection when the book has chapters (short-press Confirm); no-op otherwise
  void openChapterSelection();
  void openReaderMenu();
  void handleReaderMenuAction(int action);
  void openGoToPage();
  void confirmMarkBookCompleted();
  void renderStatusBarOverlay(const std::shared_ptr<Xtc>& book, uint32_t page, StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo(const std::shared_ptr<Xtc>& book, uint32_t page) const;
  bool saveProgress(const std::shared_ptr<Xtc>& book, uint32_t page);
  void loadProgress();
  void openReadingStats();
  void openSavedItems();
  void ensureBookmarksLoaded();
  void loadBookmarks();
  bool toggleBookmark();
  void updateCurrentPageBookmarked();
  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void finishDeferredOpenState();
  void consumeReadingViewSignal();
  void pumpDeferredCoverPreparation();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs, bool recordPace = true);
  void recordReadingSample(const ReadingSessionSample& sample, bool recordPace);
  bool refreshEstimatedTimeLeft();
  void commitReadingSession();
  void saveReadingStats();
  void markBookCompleted();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             bool completionStatsWritableAtOpen,
                             std::optional<uint32_t> initialBookmarkPage = std::nullopt,
                             int initialRefreshCountdown = 0, bool deferCoverPreparation = false,
                             bool skipStartupRecentUpdate = false)
      : Activity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        initialBookmarkPage(initialBookmarkPage),
        pagesUntilFullRefresh(initialRefreshCountdown),
        deferredCoverRequested(deferCoverPreparation),
        skipStartupRecentUpdate(skipStartupRecentUpdate),
        completionStatsWritableAtOpen(completionStatsWritableAtOpen) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
    }
    requestUpdate();
    return true;
  }
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
  bool handleReaderShortcut(uint8_t function) override;
  ScreenshotInfo getScreenshotInfo() const override;
};
