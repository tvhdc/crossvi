#pragma once

#include <Txt.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "GlobalReadingStats.h"
#include "PerBookReaderSettings.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "activities/Activity.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "clippings/ClippingPageTools.h"
#include "clippings/ClippingStore.h"

class Page;

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int lastSavedPage = -1;
  ProgressFile::WriteSession progressWriteSession;
  std::atomic<int> lastSuccessfullyRenderedPage{-1};
  int8_t pendingPageTurnDelta = 0;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  std::atomic<uint32_t> debugTurnSequence{0};
#endif
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  // Bounded, no-throw storage: firmware is built with -fno-exceptions, so an
  // allocating std::vector can abort instead of reporting low memory.
  std::unique_ptr<uint32_t[]> pageOffsets;
  size_t pageOffsetCount = 0;
  size_t pageOffsetCapacity = 0;
  bool pageIndexComplete = false;
  std::atomic<bool> pageIndexing{false};
  bool readerLayoutPrepared = false;
  std::optional<uint32_t> initialProgressOffset;
  std::vector<std::string> currentPageLines;
  std::vector<uint32_t> currentPageLineOffsets;
  HalFile contentFile;
  std::unique_ptr<uint8_t[]> pageScratch;
  size_t pageScratchSize = 0;
  std::vector<std::string> pageIndexScratchLines;
  std::vector<uint32_t> pageIndexScratchLineOffsets;
  std::optional<uint32_t> pageIndexScratchOffset;
  size_t pageIndexScratchNextOffset = 0;
  int linesPerPage = 0;
  int viewportWidth = 0;
  int cachedLineAdvance = 0;
  std::atomic<bool> initialized{false};
  bool initializationFailed = false;
  bool readerOpenStagesPending = true;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // TXT shares the same versioned, CRC-protected statistics stores as EPUB.
  // Page pace/ETA are deliberately not sampled because TXT pagination changes
  // with layout; elapsed time, sessions and page turns remain well-defined.
  BookReadingStats bookReadingStats;
  GlobalReadingStats globalReadingStats;
  bool completionStatsWritableAtOpen = true;
  bool bookReadingStatsTrusted = true;
  bool globalReadingStatsTrusted = true;
  bool bookReadingStatsWritable = true;
  bool globalReadingStatsWritable = true;
  bool completionAttemptBlocked = false;
  bool pendingStatsCompletionError = false;
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
  std::atomic<int8_t> pendingReadingViewSignal{0};
  std::atomic<uint32_t> pendingReadingViewAtMs{0};

  PerBookReaderSettings globalReaderSettings;
  PerBookReaderSettings bookReaderSettings;
  bool bookSettingsWritable = true;
  uint8_t autoPageTurnSeconds = 0;
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  bool ignoreNextConfirmRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  bool pendingShortcutUnsupportedNotice = false;
  std::atomic<bool> pendingScreenshot{false};
  bool pendingProgressSaveError = false;
  bool pendingBookSettingsSaveError = false;
  bool pendingCacheClearError = false;
  bool skipStartupRecentUpdate = false;
  bool deferredOpenStatePending = true;
  bool deferredOpenStateReady = false;
  bool readerStateSaveRetryPending = false;
  uint32_t deferredGlobalPageTurns = 0;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool bookmarksWritable = true;
  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;
  bool pendingBookmarkStorageError = false;
  unsigned long bookmarkMessageTime = 0;
  std::optional<ClippingJumpResult> initialClippingJump;
  std::optional<ProgressChangeResult> initialBookmarkJump;
  ClippingStore clippingStore;
  enum class ClippingNotice : uint8_t {
    None,
    Saved,
    LimitReached,
    SaveFailed,
    Unavailable,
    NewerFormat,
    JumpUnavailable
  };
  ClippingNotice pendingClippingNotice = ClippingNotice::None;
  bool showClippingSavedMessage = false;
  unsigned long clippingSavedMessageTime = 0;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0;

  using TextWordAnchor = ClippingPageTools::SourceWordAnchor;

  void renderCurrentPage();
  void prewarmCurrentPageFont();
  void renderCurrentPageLines() const;
  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  void finishReaderInitialization();
  bool ensureContentReadSession();
  void releasePageIndexScratch();
  void releaseContentReadSession();
  void processPageIndex();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        std::vector<uint32_t>* outLineOffsets = nullptr);
  bool loadPageAtOffsetWithScratch(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                                   std::vector<uint32_t>* outLineOffsets, HalFile& contentFile, uint8_t* buffer,
                                   size_t bufferCapacity);
  std::unique_ptr<Page> buildInteractivePage(uint16_t page, std::vector<TextWordAnchor>* anchors = nullptr);
  std::unique_ptr<Page> buildInteractivePageFromLines(const std::vector<std::string>& lines,
                                                      const std::vector<uint32_t>& lineOffsets,
                                                      std::vector<TextWordAnchor>* anchors = nullptr);
  bool buildPageIndexBatch(size_t maxPages);
  void markPageIndexFailed();
  bool appendPageOffset(uint32_t offset);
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  bool saveProgress();
  void rememberCurrentByteOffset();
  void loadProgress();
  void openReadingStats();
  void openReaderMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openBookReaderSettings();
  bool persistBookReaderSettings();
  void invalidateReaderLayout();
  void applyOrientation(uint8_t orientation);
  void updateAutoPageTurnPreference(uint8_t seconds, bool active);
  void jumpToPercent(int percent);
  void applyIndexedByteOffset(uint32_t byteOffset);
  void loadCachedBookmarks();
  bool toggleBookmark();
  void updateCurrentPageBookmarked();
  void jumpToByteOffset(uint32_t byteOffset);
  bool jumpToStoredByteOffset(uint32_t byteOffset);
  void openDictionaryWordSelect();
  void openClippingSelection();
  void openClippings();
  void openSavedItems();
  bool validateClippingJump(const ClippingJumpResult& jump) const;
  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void finishDeferredOpenState();
  void consumeReadingViewSignal();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs);
  void recordReadingSample(const ReadingSessionSample& sample);
  void commitReadingSession();
  void saveReadingStats();
  void markBookCompleted();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             bool completionStatsWritableAtOpen, PerBookReaderSettings globalReaderSettings = {},
                             PerBookReaderSettings bookReaderSettings = {}, bool bookSettingsWritable = true,
                             std::optional<ClippingJumpResult> initialClippingJump = std::nullopt,
                             std::optional<ProgressChangeResult> initialBookmarkJump = std::nullopt,
                             int initialRefreshCountdown = 0, bool skipStartupRecentUpdate = false)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        completionStatsWritableAtOpen(completionStatsWritableAtOpen),
        globalReaderSettings(std::move(globalReaderSettings)),
        bookReaderSettings(std::move(bookReaderSettings)),
        bookSettingsWritable(bookSettingsWritable),
        skipStartupRecentUpdate(skipStartupRecentUpdate),
        initialClippingJump(std::move(initialClippingJump)),
        initialBookmarkJump(std::move(initialBookmarkJump)) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return pageIndexing.load(std::memory_order_acquire); }
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
