#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>

#include <atomic>
#include <optional>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "PerBookReaderSettings.h"
#include "ProgressFile.h"
#include "ProgressMapper.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "activities/Activity.h"
#include "clippings/ClippingPageTools.h"
#include "clippings/ClippingStore.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::atomic<bool> initialCoverSkipPending{false};
  uint8_t coverSkipHops = 0;
  static constexpr uint8_t MAX_COVER_SKIP_HOPS = 8;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  ReaderUtils::X3ReaderWaveformState readerWaveform;
  // Image pages use a dedicated double-FAST path, so retain a manual refresh
  // request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedContentSourceOffset;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> pendingBookmarkSourceOffset;
  std::optional<uint32_t> currentPageSourceOffset;
  int currentPageSourceOffsetSpine = -1;
  int currentPageSourceOffsetPage = -1;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  std::atomic<bool> pendingScreenshot{false};
  bool pendingSyncSaveError = false;
  bool pendingFinishedMoveSyncError = false;
  bool pendingKOReaderCredentialsNotice = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool suppressSearchBackRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  bool bookmarksWritable = false;
  bool bookmarksLoaded = false;
  bool deferredBookmarkLoadPending = false;
  bool pendingBookmarkStorageError = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // SETTINGS is temporarily overlaid while this EPUB is active. The global
  // defaults are restored on every exit path, including sleep and sync.
  PerBookReaderSettings globalReaderSettings;
  PerBookReaderSettings bookReaderSettings;
  bool bookSettingsWritable = true;
  uint8_t autoPageTurnSeconds = 0;
  bool pendingBookSettingsSaveError = false;
  bool pendingBookStylesApplyError = false;
  bool pendingExternalCssWarning = false;
  unsigned long externalCssWarningTime = 0UL;
  bool pendingCacheClearError = false;
  bool skipStartupRecentUpdate = false;
  bool deferredOpenStatePending = true;
  bool deferredOpenStateReady = false;
  ReaderUtils::PostVisibleIdleGuard postVisibleIdleGuard;
  bool readerStateSaveRetryPending = false;
  uint32_t deferredGlobalPageTurns = 0;
  std::atomic<bool> safeModePromptRequested{false};
  std::atomic<bool> pendingSafeModeFailureNotice{false};
  // 0 = none; otherwise static_cast<uint8_t>(EpubBuildStatus) + 1.
  std::atomic<uint8_t> pendingBackgroundBuildFailure{0};
  std::atomic<bool> pendingSafeModePersistence{false};
  bool pendingSafeModeEnabledNotice = false;

  // Optional cover work begins only after the first reading page reached the
  // panel. It advances one bounded step from loop(), never before book open.
  bool deferredCoverRequested = false;
  bool deferredCoverFirstPageVisible = false;
  bool deferredCoverStarted = false;
  bool deferredCoverFinished = false;
  Epub::ThumbnailPreparationStatus deferredCoverStatus = Epub::ThumbnailPreparationStatus::NotNeeded;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  uint32_t deferredCoverStartedMs = 0;
#endif
  // At most one page ahead is scanned. One raster is streamed at a time, so
  // idle preparation has fixed memory and I/O bounds.
  int imagePrefetchSpine = -1;
  int imagePrefetchPage = -1;
  size_t imagePrefetchElement = 0;
  bool imagePrefetchPageComplete = false;
  // Retain at most one bounded, deserialized Page while the idle scanner
  // advances. It is discarded before render and whenever its target changes.
  std::unique_ptr<Page> imagePrefetchScanPage;
  uint32_t sectionGeneration = 0;
  uint32_t imagePrefetchSectionGeneration = 0;
  std::atomic<bool> imagePreparationForVisiblePage{false};
  std::string imagePreparationPath;
  bool readerOpenStagesPending = true;

  ClippingStore clippingStore;
  enum class ClippingNotice : uint8_t {
    None,
    Saved,
    LimitReached,
    SaveFailed,
    Unavailable,
    NewerFormat,
    JumpUnavailable,
    ReanchorFailed,
  };
  ClippingNotice pendingClippingNotice = ClippingNotice::None;
  bool showClippingSavedMessage = false;
  unsigned long clippingSavedMessageTime = 0UL;
  bool pendingClippingHighlightsTruncatedNotice = false;
  std::optional<ClippingJumpResult> initialClippingJump;
  std::optional<ProgressChangeResult> initialBookmarkJump;
  struct PendingClippingJump {
    uint16_t clippingIndex = 0;
    uint16_t spineIndex = 0;
    uint16_t page = 0;
    uint16_t pageCount = 1;
    uint16_t paragraphIndex = UINT16_MAX;
    uint32_t pageFingerprint = 0;
    uint32_t layoutFingerprint = 0;
    uint16_t searchFirstPage = 0;
    uint16_t searchLastPage = 0;
    int fallbackSpineIndex = 0;
    int fallbackPage = 0;
    int fallbackCachedSpineIndex = 0;
    int fallbackCachedChapterPageCount = 0;
  };
  std::optional<PendingClippingJump> pendingClippingJump;
  std::atomic<bool> pendingClippingReanchorLaunch{false};
  ClippingPageTools::HighlightNoticeTracker clippingHighlightNotices;

  BookReadingStats bookReadingStats;
  GlobalReadingStats globalReadingStats;
  bool completionStatsWritableAtOpen = true;
  // Read trust and write permission are deliberately separate. A pending
  // transaction can make valid statistics read-only; corrupt/newer files are
  // neither writable nor safe to present as real zeroes.
  bool bookReadingStatsTrusted = true;
  bool globalReadingStatsTrusted = true;
  bool bookReadingStatsWritable = true;
  bool globalReadingStatsWritable = true;
  // Prevent a broken/newer stats file or transient SD failure from causing an
  // unbounded completion retry on every loop tick. Leaving the end screen
  // permits one deliberate retry; reopening the book naturally resets it.
  bool completionAttemptBlocked = false;
  bool pendingStatsCompletionError = false;
  ReadingSessionTracker readingSessionTracker;
  uint32_t sessionReadingSeconds = 0;
  // Time-bucket/history changes are accumulated per actually visible page
  // interval and merged only if the session passes the 10-second noise filter.
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
  // render() runs on the display task; loop()/lifecycle own the tracker and
  // consume this tiny last-event-wins handoff on the main task.
  std::atomic<int8_t> pendingReadingViewSignal{0};  // -1 hidden, +1 visible
  std::atomic<uint32_t> pendingReadingViewAtMs{0};

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick. A requested page can still make one controlled retry from render().
  bool partialRebuildStartFailed = false;
  bool sectionLandingPending = false;
  bool sectionRenderWaiting = false;
  bool sectionLandingWarmupPending = false;
  // Page-turn input can arrive while a requested page is still being laid
  // out. Keep the net turn request instead of mutating the placeholder page
  // (which finishSectionLanding() would overwrite) or silently dropping it.
  std::atomic<int8_t> pendingPageTurnDelta{0};
  uint32_t sectionPrepareStartedMs = 0;

  // Reused by every grayscale page once pagination is stable. Keeping one
  // bounded 13 KiB strip avoids malloc/free churn and heap fragmentation across
  // page turns; active indexing releases it before parser allocations.
  std::unique_ptr<uint8_t[]> grayscaleStripScratch;
  size_t grayscaleStripScratchSize = 0;
  void releaseGrayscaleStripScratch();

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  std::atomic<uint32_t> debugTurnSequence{0};
  enum class DebugSectionCacheStatus : uint8_t { Unknown, Miss, Partial, Hit };
  struct DebugIndexMetrics {
    int spineIndex = -1;
    uint32_t openStartedMs = 0;
    uint32_t openStartFreeHeap = 0;
    uint32_t synchronousPages = 0;
    uint32_t backgroundPages = 0;
    uint32_t lastReportedBackgroundPages = 0;
    DebugSectionCacheStatus cacheStatus = DebugSectionCacheStatus::Unknown;
    bool waitingForVisiblePage = false;
    bool includesSwitchWait = false;
  } debugIndexMetrics;

  void debugBeginSectionOpen(bool includesSwitchWait);
  void debugSetSectionCacheStatus(DebugSectionCacheStatus status);
  void debugRecordSectionBuild(uint16_t before, bool background);
  void debugReportVisibleSection();
  void debugReportCompletedBuild() const;
#endif

  // The render task records only positions that actually reached the panel.
  // The main task coalesces rapid turns and publishes the newest snapshot once
  // the turn queue is idle. Lifecycle callbacks run under RenderLock and force
  // one final attempt before the book is hidden or released.
  struct PendingProgressSave {
    bool active = false;
    bool retryBlocked = false;
    int spineIndex = -1;
    int page = -1;
    int pageCount = -1;
    std::optional<uint32_t> visibleTextOffset;
    uint32_t stagedAtMs = 0;
  } pendingProgressSave;
  static constexpr uint32_t PROGRESS_SAVE_IDLE_MS = 1000;

  // Last position successfully persisted, used to skip no-op re-renders.
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;
  ProgressFile::WriteSession progressWriteSession;

  std::optional<bool> renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                                     int orientedMarginBottom, int orientedMarginLeft, uint32_t* pageFingerprintOut);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump. Kept at one so a build chunk
  // never noticeably delays input or a pending render; fresh landings wait for
  // their target-relative buffer cooperatively instead of building it in one
  // render callback.
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;
  static constexpr int BACKGROUND_BUILD_PARSE_STEPS_PER_TICK = 1;
  // Background parsing grows vectors/strings through throwing allocation
  // paths. Defer optional build ticks before fragmented heap reaches OOM.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Restore the cached content position after a settings change re-paginates a chapter.
  // Falls back to the old page ratio only when the visible page has no stable text anchor.
  bool applyDeferredReposition();
  void clearDeferredReposition();
  bool sectionTurnBufferReady(int targetPage) const;
  std::optional<int> sectionLandingTargetPage() const;
  bool sectionLandingReady() const;
  bool sectionLandingReadyForRender() const;
  bool requestedSectionTargetReady() const;
  bool requestedSectionPageReady() const;
  void finishSectionLanding();
  void rememberCurrentContentOffset();
  void stageProgressSave(int spineIndex, int currentPage, int pageCount);
  bool flushPendingProgressSave();
  bool writeProgress(int spineIndex, int currentPage, int pageCount, const std::optional<uint32_t>& visibleTextOffset);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  bool launchNearbyPositionSync();
  void applyOrientation(uint8_t orientation);
  void openBookReaderSettings();
  void openReadingStats();
  void openClippingSelection();
  void openClippings();
  void openSavedItems();
  void applyBookmarkJump(const ProgressChangeResult& progress);
  uint32_t currentClippingLayoutFingerprint() const;
  bool validateClippingJump(const ClippingJumpResult& jump) const;
  void armClippingJump(const ClippingJumpResult& jump);
  bool abortPendingClippingJump(bool showNotice = true);
  // Returns true when an invalid target restored the previous section and this
  // render must restart there. The page fingerprint is intentionally checked
  // later against the exact Page instance that will be rendered.
  bool preparePendingClippingJump();
  void launchPendingClippingReanchor();
  bool persistBookReaderSettings();
  bool queueSafeModePromptIfEligible(EpubBuildStatus status);
  void invalidateReaderLayout();
  void applyAutoPageTurnRuntime(uint8_t seconds, bool active);
  void updateAutoPageTurnPreference(uint8_t seconds, bool active);
  void pageTurn(bool isForwardTurn, bool queueWhileWaiting = true, bool drainingQueuedTurn = false);
  bool retargetQueuedPageTurns();
  bool moveOnePageWithoutRendering(bool forward);
  bool skipCoverPageIfNeeded(const Page& page);
  void loadCachedBookmarks();
  void ensureBookmarksLoaded();
  void pumpDeferredBookmarkLoad();
  bool addBookmark();
  void updateBookmarkFlag();

  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void finishDeferredOpenState();
  void consumeReadingViewSignal();
  void pumpDeferredCoverPreparation();
  bool pumpImagePreparation();
  void queueVisiblePageImagePreparation();
  void resetImagePageScan();
  void cancelImagePreparation();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs);
  void recordReadingSample(const ReadingSessionSample& sample);
  void commitReadingSession();
  void saveReadingStats();
  bool refreshEstimatedTimeLeft();
  void markBookCompleted();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              bool completionStatsWritableAtOpen, PerBookReaderSettings globalReaderSettings,
                              PerBookReaderSettings bookReaderSettings, bool bookSettingsWritable,
                              std::optional<ClippingJumpResult> initialClippingJump = std::nullopt,
                              std::optional<ProgressChangeResult> initialBookmarkJump = std::nullopt,
                              int initialRefreshCountdown = 0, bool deferCoverPreparation = false,
                              bool skipStartupRecentUpdate = false)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        globalReaderSettings(std::move(globalReaderSettings)),
        bookReaderSettings(std::move(bookReaderSettings)),
        bookSettingsWritable(bookSettingsWritable),
        skipStartupRecentUpdate(skipStartupRecentUpdate),
        deferredCoverRequested(deferCoverPreparation),
        initialClippingJump(std::move(initialClippingJump)),
        initialBookmarkJump(std::move(initialBookmarkJump)),
        completionStatsWritableAtOpen(completionStatsWritableAtOpen) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed only while the incremental builder can make progress. Once
  // the five-page window is full, normal loop delay saves power until the
  // reader advances and opens more work for the builder.
  bool skipLoopDelay() override {
    const bool requiredBuild = sectionLandingPending || sectionRenderWaiting;
    const bool building = section && section->isBuilding() &&
                          (requiredBuild || section->isPartial() ||
                           static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
                          (requiredBuild || !buildHeapPaused);
    const bool preparingCover = deferredCoverRequested && deferredCoverFirstPageVisible && !deferredCoverFinished;
    const bool preparingImage =
        imagePreparationForVisiblePage.load(std::memory_order_acquire) || (epub && epub->imagePreparationActive());
    return building || preparingImage || preparingCover;
  }
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = -1;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
  bool handleReaderShortcut(uint8_t function) override;
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
