#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/PageSourceAnchor.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>

#include "BookReaderSettingsActivity.h"
#include "BookSavedItemsActivity.h"
#include "BookmarkEntry.h"
#include "ClipSelectionActivity.h"
#include "ClippingListActivity.h"
#include "ClippingReanchorActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubInBookSearchActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "NearbyPositionSyncActivity.h"
#include "PerBookReaderSettingsBridge.h"
#include "PerBookReaderSettingsStore.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsDateEditActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "clippings/ClippingJumpValidation.h"
#include "clippings/ClippingPageTools.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"
#include "util/BookmarkUtil.h"
#include "util/DictionaryHistoryStore.h"
#include "util/ScreenshotUtil.h"

namespace {
bool extractEpubImage(void* context, const char* sourcePath, const char* destinationPath) {
  const auto* epub = static_cast<const Epub*>(context);
  return epub && sourcePath && destinationPath && epub->extractItemToFileAtomically(sourcePath, destinationPath);
}

// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr uint8_t AUTO_PAGE_TURN_MIN_SECONDS = 5;
constexpr uint8_t AUTO_PAGE_TURN_MAX_SECONDS = 120;
constexpr unsigned long MILLISECONDS_PER_SECOND = 1000UL;
static_assert(AUTO_PAGE_TURN_MAX_SECONDS <= std::numeric_limits<unsigned long>::max() / MILLISECONDS_PER_SECOND);
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

uint8_t normalizeAutoPageTurnSeconds(const uint8_t seconds) {
  return seconds >= AUTO_PAGE_TURN_MIN_SECONDS && seconds <= AUTO_PAGE_TURN_MAX_SECONDS ? seconds : 0;
}

EpubRenderMode activeEpubRenderMode() {
  if (SETTINGS.epubSafeMode) return EpubRenderMode::Light;
  return isValidEpubRenderMode(SETTINGS.epubRenderMode) ? static_cast<EpubRenderMode>(SETTINGS.epubRenderMode)
                                                        : EpubRenderMode::Balanced;
}

bool activeEmbeddedStyle() { return SETTINGS.epubSafeMode == 0 && SETTINGS.embeddedStyle != 0; }

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange,
                             const std::optional<uint32_t> sourceOffset = std::nullopt) {
  if (bookmark.hasContentSourceOffset && sourceOffset.has_value()) {
    return bookmark.computedSpineIndex == spineIndex && bookmark.contentSourceOffset == *sourceOffset;
  }
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and all path-keyed user state into /read/.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (moveBookFilePreservingUserState(srcPath, dstPath) != BookPathMoveResult::Moved) {
    LOG_ERR("ERS", "Finished book and its user state could not be moved safely");
  }
}

}  // namespace

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
void EpubReaderActivity::debugBeginSectionOpen(const bool includesSwitchWait) {
  if (debugIndexMetrics.waitingForVisiblePage && debugIndexMetrics.spineIndex == currentSpineIndex) {
    debugIndexMetrics.includesSwitchWait = debugIndexMetrics.includesSwitchWait || includesSwitchWait;
    return;
  }

  debugIndexMetrics = {};
  debugIndexMetrics.spineIndex = currentSpineIndex;
  debugIndexMetrics.openStartedMs = static_cast<uint32_t>(millis());
  debugIndexMetrics.openStartFreeHeap = ESP.getFreeHeap();
  debugIndexMetrics.waitingForVisiblePage = true;
  debugIndexMetrics.includesSwitchWait = includesSwitchWait;
}

void EpubReaderActivity::debugSetSectionCacheStatus(const DebugSectionCacheStatus status) {
  debugIndexMetrics.cacheStatus = status;
}

void EpubReaderActivity::debugRecordSectionBuild(const uint16_t before, const bool background) {
  if (!section) return;
  const uint16_t after = section->debugBuiltPageCount();
  const uint32_t built = after >= before ? static_cast<uint32_t>(after - before) : 0;
  if (built == 0) {
    if (section->isBuildComplete()) debugReportCompletedBuild();
    return;
  }

  uint32_t& total = background ? debugIndexMetrics.backgroundPages : debugIndexMetrics.synchronousPages;
  total += built;

  if (section->isBuildComplete()) {
    debugReportCompletedBuild();
    return;
  }

  // A partial rebuild can span hundreds of pages. Report coarse progress rather
  // than emitting a serial line for every two-page background pump.
  if (background && total - debugIndexMetrics.lastReportedBackgroundPages >= 16) {
    debugIndexMetrics.lastReportedBackgroundPages = total;
    LOG_DBG("IDX", "Section build progress: spine=%d background_pages=%u sync_pages=%u free_heap=%u", currentSpineIndex,
            static_cast<unsigned>(total), static_cast<unsigned>(debugIndexMetrics.synchronousPages),
            static_cast<unsigned>(ESP.getFreeHeap()));
  } else if (!background && !debugIndexMetrics.waitingForVisiblePage) {
    LOG_DBG("IDX", "Section render catch-up: spine=%d pages=%u sync_total=%u free_heap=%u", currentSpineIndex,
            static_cast<unsigned>(built), static_cast<unsigned>(total), static_cast<unsigned>(ESP.getFreeHeap()));
  }
}

void EpubReaderActivity::debugReportVisibleSection() {
  if (!debugIndexMetrics.waitingForVisiblePage || debugIndexMetrics.spineIndex != currentSpineIndex || !section) return;

  const char* cache = "unknown";
  switch (debugIndexMetrics.cacheStatus) {
    case DebugSectionCacheStatus::Miss:
      cache = "miss";
      break;
    case DebugSectionCacheStatus::Partial:
      cache = "partial";
      break;
    case DebugSectionCacheStatus::Hit:
      cache = "hit";
      break;
    case DebugSectionCacheStatus::Unknown:
      break;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  const int32_t heapDelta = static_cast<int32_t>(debugIndexMetrics.openStartFreeHeap) - static_cast<int32_t>(freeHeap);
  LOG_DBG("IDX",
          "Section visible: spine=%d cache=%s switch_wait=%u elapsed_ms=%u sync_pages=%u background_pages=%u "
          "page=%d/%u heap_delta=%ld free_heap=%u min_free_heap=%u",
          currentSpineIndex, cache, debugIndexMetrics.includesSwitchWait ? 1U : 0U,
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - debugIndexMetrics.openStartedMs),
          static_cast<unsigned>(debugIndexMetrics.synchronousPages),
          static_cast<unsigned>(debugIndexMetrics.backgroundPages), section->currentPage + 1,
          static_cast<unsigned>(section->pageCount), static_cast<long>(heapDelta), static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
  debugIndexMetrics.waitingForVisiblePage = false;
}

void EpubReaderActivity::debugReportCompletedBuild() const {
  LOG_DBG("IDX",
          "Section build complete: spine=%d sync_pages=%u background_pages=%u total_pages=%u free_heap=%u "
          "min_free_heap=%u",
          currentSpineIndex, static_cast<unsigned>(debugIndexMetrics.synchronousPages),
          static_cast<unsigned>(debugIndexMetrics.backgroundPages),
          section ? static_cast<unsigned>(section->pageCount) : 0U, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
}
#endif

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Keep an EPUB readable when optional publisher CSS is unsupported or the
  // SD read fails, but never make that lower-fidelity fallback invisible.
  pendingExternalCssWarning = epub->isExternalCssUnavailable();

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), extractEpubImage);

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  const ClippingStore::LoadResult clippingLoad =
      clippingStore.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor());
  if (!clippingStore.isLoaded()) {
    LOG_ERR("ERS", "Clipping store unavailable (status %u)", static_cast<unsigned>(clippingLoad));
  }

  const ReadingStatsCompletionTransaction::RecoveryResult completionRecovery =
      ReadingStatsCompletionTransaction::recoverPending();
  const bool completionStatsWritable = completionRecovery != ReadingStatsCompletionTransaction::RecoveryResult::Blocked;

  BookReadingStats::LoadStatus bookStatsStatus = BookReadingStats::LoadStatus::Missing;
  bookReadingStats = BookReadingStats::load(epub->getCachePath(), &bookStatsStatus);
  bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(bookStatsStatus);
  bookReadingStatsWritable =
      completionStatsWritable && bookReadingStatsTrusted && BookReadingStats::canPublish(epub->getCachePath());
  GlobalReadingStats::LoadStatus globalStatsStatus = GlobalReadingStats::LoadStatus::Missing;
  globalReadingStats = GlobalReadingStats::load(&globalStatsStatus);
  globalReadingStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(globalStatsStatus);
  // load() already performed the root-level future-version/protection scan.
  // Every actual save and completion transaction revalidates immediately
  // before publication, so repeating the whole /.crosspoint scan here adds
  // library-size-dependent latency without creating a safety boundary.
  globalReadingStatsWritable = completionStatsWritable && globalReadingStatsTrusted;
  readingSessionTracker = ReadingSessionTracker{};
  sessionReadingSeconds = 0;
  pendingBookReadingSpans = {};
  pendingGlobalReadingSpans = {};
  hasActiveReadingSpanStartLocalDateTime = false;
  hasSessionStartLocalDateTime = false;
  readingSessionCommitted = false;
  bookReadingStatsDirty = false;
  globalReadingStatsDirty = false;
  pendingReadingViewSignal.store(0, std::memory_order_relaxed);
  confirmHold.reset();
  pageTurnGesture.reset();
  ignoreNextConfirmRelease = false;

  const uint8_t configuredAutoPageTurnSeconds =
      bookReaderSettings.hasAutoPageTurnInterval ? bookReaderSettings.autoPageTurnSeconds : 0;
  applyAutoPageTurnRuntime(configuredAutoPageTurnSeconds, bookReaderSettings.autoPageTurnStartsOnOpen);

  uint8_t data[6]{};
  const int spineCount = epub->getSpineItemsCount();
  const ProgressFile::EpubBounds progressBounds{spineCount > 0 ? static_cast<uint32_t>(spineCount) : 0};
  const ProgressFile::CandidateValidator progressValidator{ProgressFile::validateEpubBounds, &progressBounds};
  const ProgressFile::LoadResult progress =
      ProgressFile::loadEpub(epub->getCachePath(), data, sizeof(data), progressValidator);
  if (progress) {
    const size_t dataSize = progress.size;
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    lastSavedSpineIndex = currentSpineIndex;
    lastSavedPage = nextPageNumber;
    lastSavedPageCount = cachedChapterTotalPageCount;
  } else if (progress.source == ProgressFile::LoadSource::Invalid ||
             progress.source == ProgressFile::LoadSource::IoError) {
    LOG_ERR("ERS", "No valid progress copy could be read");
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // A device-wide request is intentionally applied only after normal progress
  // has been loaded. That progress remains the rollback position until the
  // target page itself passes the final rendered-page fingerprint check.
  if (initialClippingJump) {
    if (validateClippingJump(*initialClippingJump)) {
      armClippingJump(*initialClippingJump);
    } else {
      pendingClippingNotice = ClippingNotice::JumpUnavailable;
    }
    initialClippingJump.reset();
  }
  if (initialBookmarkJump) {
    const bool valid = initialBookmarkJump->spineIndex >= 0 && initialBookmarkJump->spineIndex < spineCount &&
                       initialBookmarkJump->page >= 0 && initialBookmarkJump->totalPages >= 0 &&
                       (!initialBookmarkJump->hasSavedProgress ||
                        (std::isfinite(initialBookmarkJump->percentage) && initialBookmarkJump->percentage >= 0.0f &&
                         initialBookmarkJump->percentage <= 1.0f));
    if (valid) {
      applyBookmarkJump(*initialBookmarkJump);
    } else {
      pendingClippingNotice = ClippingNotice::JumpUnavailable;
    }
    initialBookmarkJump.reset();
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Clear the non-owning lazy-image callback before releasing the Epub.
  ImageBlock::setExtractor(nullptr, nullptr);

  commitReadingSession();
  saveReadingStats();
  DICTIONARY_HISTORY.flush();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // The per-book overlay must never leak into settings.json or the next book.
  applyReaderSettings(globalReaderSettings);
  // The global profile is active again, so a genuinely missing global SD font
  // may now be repaired in settings.json without persisting any book overlay.
  sdFontSystem.ensureLoaded(renderer);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    clippingStore.unload();
    moveFinishedBookToReadFolder(srcPath, dstPath);
  } else {
    epub.reset();
  }
  clippingStore.unload();
}

void EpubReaderActivity::onPause() {
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
}

void EpubReaderActivity::onResume() {
  // A child leaves its own pixels on the panel until the reader redraws. Only
  // that successful redraw is allowed to restart active reading time.
  pendingReadingViewSignal.store(0, std::memory_order_release);
  pageTurnGesture.reset();
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmHold.reset();
    ignoreNextConfirmRelease = false;
  }
}

void EpubReaderActivity::signalReadingPageVisible() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(1, std::memory_order_release);
}

void EpubReaderActivity::signalReadingPageHidden() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(-1, std::memory_order_release);
}

void EpubReaderActivity::consumeReadingViewSignal() {
  const int8_t signal = pendingReadingViewSignal.exchange(0, std::memory_order_acq_rel);
  if (signal == 0) return;

  const uint32_t eventAtMs = pendingReadingViewAtMs.load(std::memory_order_relaxed);
  if (signal > 0) {
    deferredCoverFirstPageVisible = true;
    if (readingSessionTracker.pageVisible(eventAtMs)) {
      ReadingStatsDateTime localStart;
      hasActiveReadingSpanStartLocalDateTime = getCurrentLocalReadingStatsDateTime(localStart);
      if (hasActiveReadingSpanStartLocalDateTime) {
        activeReadingSpanStartLocalDateTime = localStart;
        if (!hasSessionStartLocalDateTime) {
          sessionStartLocalDateTime = localStart;
          hasSessionStartLocalDateTime = true;
        }
      }
    }
  } else {
    stopReadingPage(false, eventAtMs);
  }
}

void EpubReaderActivity::pumpDeferredCoverPreparation() {
  if (!epub || !deferredCoverRequested || !deferredCoverFirstPageVisible || deferredCoverFinished) return;

  const bool needsShared = CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel = CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  const Epub::ThumbnailRequest request{needsShared, needsCarousel, renderer.getDisplayHeight() == 528};
  if (!needsShared && !needsCarousel) {
    deferredCoverFinished = true;
    return;
  }

  if (!deferredCoverStarted) {
    deferredCoverStatus = epub->beginThumbnailPreparation(request);
    deferredCoverStarted = true;
  } else if (deferredCoverStatus == Epub::ThumbnailPreparationStatus::InProgress) {
    deferredCoverStatus = epub->stepThumbnailPreparation();
  }

  if (deferredCoverStatus == Epub::ThumbnailPreparationStatus::InProgress) return;

  if (deferredCoverStatus == Epub::ThumbnailPreparationStatus::Ready ||
      deferredCoverStatus == Epub::ThumbnailPreparationStatus::NeedsSynchronousGeneration ||
      deferredCoverStatus == Epub::ThumbnailPreparationStatus::NotNeeded) {
    const Epub::ThumbnailSetStatus status = epub->ensureThumbnails(request);
    const auto settled = [](const Epub::ThumbnailStatus value) {
      return value == Epub::ThumbnailStatus::Ready || value == Epub::ThumbnailStatus::NoCover;
    };
    if ((needsShared && !settled(status.shared)) || (needsCarousel && !settled(status.carousel))) {
      LOG_ERR("ERS", "Deferred cover preparation did not produce every requested thumbnail");
    }
  } else {
    LOG_ERR("ERS", "Deferred cover source preparation failed");
  }
  deferredCoverFinished = true;
}

void EpubReaderActivity::recordReadingSample(const ReadingSessionSample& sample) {
  if (sample.seconds > 0) {
    sessionReadingSeconds = addReadingStatsSaturated(sessionReadingSeconds, sample.seconds);
  }
  if (!sample.forwardPageRead) return;

  if (bookReadingStatsWritable) {
    bookReadingStats.totalPagesTurned = addReadingStatsSaturated(bookReadingStats.totalPagesTurned, 1);
    bookReadingStats.recordForwardPageRead(sample.seconds);
    bookReadingStatsDirty = true;
  }
  if (globalReadingStatsWritable) {
    globalReadingStats.totalPagesTurned = addReadingStatsSaturated(globalReadingStats.totalPagesTurned, 1);
    globalReadingStatsDirty = true;
  }
}

void EpubReaderActivity::stopReadingPage(const bool forwardPageTurn, const uint32_t nowMs) {
  const ReadingSessionSample sample = readingSessionTracker.stop(nowMs, forwardPageTurn);
  if (sample.seconds > 0 && hasActiveReadingSpanStartLocalDateTime) {
    pendingBookReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
    pendingGlobalReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
  }
  hasActiveReadingSpanStartLocalDateTime = false;
  recordReadingSample(sample);
}

bool EpubReaderActivity::refreshEstimatedTimeLeft() {
  if (!bookReadingStatsWritable) return false;
  if (bookReadingStats.isCompleted) {
    if (bookReadingStats.estimatedTimeLeftSeconds != 0) {
      bookReadingStats.estimatedTimeLeftSeconds = 0;
      bookReadingStatsDirty = true;
    }
    return false;
  }

  // A partial/in-progress section only knows its current watermark, not its
  // final page count. Keep the previous estimate until pagination is stable.
  if (!epub || !section || section->isBuilding() || section->isPartial() || section->pageCount <= 0 ||
      bookReadingStats.paceSampleCount < 3 || bookReadingStats.avgSecondsPerForwardPage == 0 ||
      epub->getBookSize() == 0) {
    return false;
  }

  const size_t previousChapterBytes =
      currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t cumulativeChapterBytes = epub->getCumulativeSpineItemSize(currentSpineIndex);
  if (cumulativeChapterBytes <= previousChapterBytes) return false;

  const double chapterBytes = static_cast<double>(cumulativeChapterBytes - previousChapterBytes);
  const double bytesPerPage = chapterBytes / static_cast<double>(section->pageCount);
  const double completedBytes =
      static_cast<double>(previousChapterBytes) + static_cast<double>(section->currentPage) * bytesPerPage;
  if (bytesPerPage <= 0.0 || completedBytes >= static_cast<double>(epub->getBookSize())) return false;

  const double remainingPages = (static_cast<double>(epub->getBookSize()) - completedBytes) / bytesPerPage;
  const double estimate = remainingPages * static_cast<double>(bookReadingStats.avgSecondsPerForwardPage);
  const uint32_t estimatedSeconds = estimate >= static_cast<double>(std::numeric_limits<uint32_t>::max())
                                        ? std::numeric_limits<uint32_t>::max()
                                        : static_cast<uint32_t>(estimate + 0.5);
  if (estimatedSeconds > 0 && estimatedSeconds != bookReadingStats.estimatedTimeLeftSeconds) {
    bookReadingStats.estimatedTimeLeftSeconds = estimatedSeconds;
    bookReadingStatsDirty = true;
  }
  return estimatedSeconds > 0;
}

void EpubReaderActivity::commitReadingSession() {
  if (readingSessionCommitted) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  readingSessionCommitted = true;

  // Match CrossInk v1.4.0: ten active seconds contribute reading time, while a
  // visit must reach one minute before it counts as a reading session.
  if (sessionReadingSeconds >= 60) {
    if (bookReadingStatsWritable) {
      if (bookReadingStats.sessionCount < std::numeric_limits<uint16_t>::max()) {
        ++bookReadingStats.sessionCount;
      }
      bookReadingStatsDirty = true;
    }
    if (globalReadingStatsWritable) {
      globalReadingStats.totalSessions = addReadingStatsSaturated(globalReadingStats.totalSessions, 1);
      if (hasSessionStartLocalDateTime) globalReadingStats.recordReadingSession(sessionStartLocalDateTime.date);
      globalReadingStatsDirty = true;
    }
  }

  if (sessionReadingSeconds >= 10) {
    if (bookReadingStatsWritable) {
      bookReadingStats.totalReadingSeconds =
          addReadingStatsSaturated(bookReadingStats.totalReadingSeconds, sessionReadingSeconds);
      for (size_t i = 0; i < bookReadingStats.timeOfDaySeconds.size(); ++i) {
        bookReadingStats.timeOfDaySeconds[i] =
            addReadingStatsSaturated(bookReadingStats.timeOfDaySeconds[i], pendingBookReadingSpans.timeOfDaySeconds[i]);
      }
      for (size_t i = 0; i < bookReadingStats.dayOfWeekSeconds.size(); ++i) {
        bookReadingStats.dayOfWeekSeconds[i] =
            addReadingStatsSaturated(bookReadingStats.dayOfWeekSeconds[i], pendingBookReadingSpans.dayOfWeekSeconds[i]);
      }
      if (sessionReadingSeconds >= 120 && hasSessionStartLocalDateTime && !bookReadingStats.startDateManual &&
          !bookReadingStats.startDate.isValid()) {
        bookReadingStats.startDate = sessionStartLocalDateTime.date;
        bookReadingStats.startMinuteOfDay =
            static_cast<uint16_t>(sessionStartLocalDateTime.hour) * 60u + sessionStartLocalDateTime.minute;
      }
      bookReadingStatsDirty = true;
    }
    if (globalReadingStatsWritable) {
      globalReadingStats.totalReadingSeconds =
          addReadingStatsSaturated(globalReadingStats.totalReadingSeconds, sessionReadingSeconds);
      globalReadingStats.merge(pendingGlobalReadingSpans);
      globalReadingStats.longestReadingStreak = globalReadingStats.displayLongestReadingStreak();
      globalReadingStatsDirty = true;
    }
  }
  refreshEstimatedTimeLeft();
}

void EpubReaderActivity::saveReadingStats() {
  if (bookReadingStatsWritable && bookReadingStatsDirty && epub) {
    if (bookReadingStats.save(epub->getCachePath())) {
      bookReadingStatsDirty = false;
    } else {
      LOG_ERR("ERS", "Failed to save book reading statistics");
    }
  }
  if (globalReadingStatsWritable && globalReadingStatsDirty) {
    if (globalReadingStats.save()) {
      globalReadingStatsDirty = false;
    } else {
      LOG_ERR("ERS", "Failed to save global reading statistics");
    }
  }
}

void EpubReaderActivity::markBookCompleted() {
  if (!epub || bookReadingStats.isCompleted || completionAttemptBlocked) return;
  const auto reportFailure = [this]() {
    completionAttemptBlocked = true;
    pendingStatsCompletionError = true;
    requestUpdate();
  };
  if (!bookReadingStatsWritable || !globalReadingStatsWritable) {
    LOG_ERR("ERS", "Could not mark the book complete because reading statistics are protected or unreadable");
    reportFailure();
    return;
  }

  // Completion is the only stats action that must coordinate two files. Flush
  // ordinary session/page counters first so the transaction's old snapshots
  // exactly match storage and can be recovered without guessing.
  saveReadingStats();
  if (bookReadingStatsDirty || globalReadingStatsDirty) {
    LOG_ERR("ERS", "Could not flush reading statistics before marking the book complete");
    reportFailure();
    return;
  }

  BookReadingStats completedBookStats = bookReadingStats;
  GlobalReadingStats completedGlobalStats = globalReadingStats;
  completedBookStats.isCompleted = true;
  completedBookStats.estimatedTimeLeftSeconds = 0;
  if (!completedBookStats.finishedDateManual && !completedBookStats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      completedBookStats.finishedDate = now.date;
      completedBookStats.finishedMinuteOfDay = static_cast<uint16_t>(now.hour) * 60u + now.minute;
    }
  }
  completedGlobalStats.completedBooks = addReadingStatsSaturated(completedGlobalStats.completedBooks, 1);
  if (!ReadingStatsCompletionTransaction::commit(epub->getCachePath(), bookReadingStats, completedBookStats,
                                                 globalReadingStats, completedGlobalStats)) {
    LOG_ERR("ERS", "Could not commit book completion statistics");
    bookReadingStatsWritable = false;
    globalReadingStatsWritable = false;
    reportFailure();
    return;
  }
  bookReadingStats = completedBookStats;
  globalReadingStats = completedGlobalStats;
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, autoPageTurnSeconds, automaticPageTurnActive,
                             !currentPageFootnotes.empty(), !cachedBookmarks.empty(), currentPageBookmarked,
                             EpubReaderMenuActivity::ReaderKind::Epub, clippingStore.isLoaded(),
                             clippingStore.isLoaded() && clippingStore.size() > 0, epub->getTocItemsCount() > 0,
                             bookReadingStats.isCompleted),
                         [this](const ActivityResult& result) {
                           const auto* menu = std::get_if<MenuResult>(&result.data);
                           if (!menu) {
                             LOG_ERR("ERS", "Reader menu returned an unexpected result type");
                             requestUpdate();
                             return;
                           }
                           // Always apply orientation change even if the menu was cancelled
                           applyOrientation(menu->orientation);
                           if (menu->autoPageTurnChanged) updateAutoPageTurnFromMenu(menu->autoPageTurnSeconds);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu->action));
                           }
                         });
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  // Word geometry must match render(): viewable-area margins plus screen margin.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::loop() {
  consumeReadingViewSignal();
  if (readingSessionTracker.discardIfIdle(static_cast<uint32_t>(millis()))) {
    hasActiveReadingSpanStartLocalDateTime = false;
    LOG_DBG("ERS", "Reading interval discarded after idle threshold");
  }

  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (safeModePromptRequested.exchange(false, std::memory_order_acq_rel)) {
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_EPUB_SAFE_MODE),
                                                                  tr(STR_EPUB_SAFE_MODE_PROMPT)),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               pendingSafeModeFailureNotice.store(true, std::memory_order_release);
                               requestUpdate();
                               return;
                             }

                             bookReaderSettings.safeModeEnabled = true;
                             applyEffectiveBookReaderSettings(globalReaderSettings, bookReaderSettings);
                             pendingSafeModePersistence.store(true, std::memory_order_release);
                             invalidateReaderLayout();
                             requestUpdate();
                           });
    return;
  }

  if (pendingClippingReanchorLaunch.exchange(false, std::memory_order_acq_rel)) {
    launchPendingClippingReanchor();
    return;
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  {
    RenderLock lock(std::try_to_lock);
    if (lock.ownsLock() && section && !section->isBuilding() && section->isPartial() && buildViewportWidth > 0 &&
        !partialRebuildStartFailed &&
        section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
      if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, buildViewportWidth,
                               buildViewportHeight, SETTINGS.hyphenationEnabled, activeEmbeddedStyle(),
                               SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing,
                               activeEpubRenderMode(),
                               SETTINGS.forceParagraphIndents != 0)) {
        const EpubBuildStatus failure = section->lastBuildStatus();
        if (queueSafeModePromptIfEligible(failure)) {
          stopReadingPage(false, static_cast<uint32_t>(millis()));
          return;
        }
        // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
        // the blocking extension in render(). Don't retry every tick.
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
      }
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  {
    RenderLock lock(std::try_to_lock);
    if (lock.ownsLock() && section && section->isBuilding() &&
        (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
        buildTickHeapGate()) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      const uint16_t debugPagesBefore = section->debugBuiltPageCount();
#endif
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        const EpubBuildStatus failure = section->lastBuildStatus();
        stopReadingPage(false, static_cast<uint32_t>(millis()));
        section.reset();
        if (queueSafeModePromptIfEligible(failure)) return;
        requestUpdate();
      } else {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        debugRecordSectionBuild(debugPagesBefore, true);
#endif
        if (section->isBuildComplete()) {
          // Finalization can happen entirely in the background while the visible
          // page stays unchanged. Persist the now-exact total here; otherwise an
          // immediate Home action can leave progress.bin carrying the earlier
          // estimate and Dashboard correctly refuses to display it.
          const bool repositioned = applyDeferredReposition();
          const int exactPageCount = section->pageCount;
          if (saveProgress(currentSpineIndex, section->currentPage, exactPageCount)) {
            lastSavedSpineIndex = currentSpineIndex;
            lastSavedPage = section->currentPage;
            lastSavedPageCount = exactPageCount;
          } else {
            pendingSyncSaveError = true;
          }
          if (repositioned || pendingSyncSaveError) requestUpdate();
        }
      }
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  if (atEndOfBook) {
    markBookCompleted();
  } else {
    completionAttemptBlocked = false;
  }

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    // Never move the cache path named by an unresolved completion marker.
    pendingReadFolderMove = bookReadingStatsWritable && globalReadingStatsWritable && bookReadingStats.isCompleted &&
                            SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHold.onPress();
  const bool suppressConfirmRelease =
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) && ignoreNextConfirmRelease;
  if (suppressConfirmRelease) {
    confirmHold.onRelease();
    ignoreNextConfirmRelease = false;
  }

  if (automaticPageTurnActive) {
    if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !suppressConfirmRelease) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      applyAutoPageTurnRuntime(autoPageTurnSeconds, false);
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  if (pendingExternalCssWarning && externalCssWarningTime != 0 &&
      (millis() - externalCssWarningTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    pendingExternalCssWarning = false;
    externalCssWarningTime = 0;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() && !suppressConfirmRelease &&
      !ReaderUtils::isLongPageTurnRelease(mappedInput, pageTurnGesture)) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage: {
        RenderLock lock(*this);
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        lock.unlock();
        requestUpdate();
        return;
      }
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Long-press Confirm runs the selected shortcut once at the 500 ms threshold.
  if (SETTINGS.longPressMenuFunction != CrossPointSettings::LP_MENU_DISABLED &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      confirmHold.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Confirm), ReaderUtils::CONFIRM_HOLD_MS)) {
    ignoreNextConfirmRelease = true;
    if (handleReaderShortcut(SETTINGS.longPressMenuFunction)) return;
  }

  // A handled hold consumes its release; an ordinary release opens the reader menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (suppressConfirmRelease) return;
    if (confirmHold.onRelease() == ReaderUtils::HoldRelease::Short) {
      openReaderMenu();
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Back) < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (ReaderUtils::handleBackNavigation(
          mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
          {nullptr, [](void*) { activityManager.returnFromReaderOrHome(); }},
          {this, [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->showReaderExitFeedback(); }})) {
    return;
  }

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto pageGesture = ReaderUtils::detectPageTurnGesture(mappedInput, pageTurnGesture);
  const bool prevTriggered = pageGesture.prev;
  const bool nextTriggered = pageGesture.next;
  const bool longPress = pageGesture.longPress;
  if (!prevTriggered && !nextTriggered) {
    // Input handling above always wins. Cover extraction advances at most one
    // bounded chunk per idle loop after the first page has reached the panel.
    pumpDeferredCoverPreparation();
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      {
        RenderLock lock(*this);
        currentSpineIndex = epub->getSpineItemsCount() - 1;
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
      }
      requestUpdate();
    }
    return;
  }

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    consumeReadingViewSignal();
    stopReadingPage(false, static_cast<uint32_t>(millis()));
    {
      RenderLock lock(*this);
      if (!nextTriggered && section && section->currentPage > 0) {
        section->currentPage = 0;
      } else {
        nextPageNumber = 0;
        if (nextTriggered) {
          currentSpineIndex++;
        } else if (currentSpineIndex > 0) {
          currentSpineIndex--;
        }
        section.reset();
      }
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

bool EpubReaderActivity::handleReaderShortcut(const uint8_t function) {
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_FUNCTION>(function)) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
      showBookmarkMessage = addBookmark();
      if (showBookmarkMessage) bookmarkMessageTime = millis();
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_KOSYNC:
      if (!launchKOReaderSync()) {
        pendingKOReaderCredentialsNotice = true;
        requestUpdate();
      }
      return true;
    case CrossPointSettings::LP_MENU_DICTIONARY:
      openDictionaryWordSelect();
      return true;
    case CrossPointSettings::LP_MENU_READING_STATS:
      openReadingStats();
      return true;
    case CrossPointSettings::LP_MENU_AUTO_PAGE_TURN:
      applyAutoPageTurnRuntime(ReaderUtils::autoPageTurnShortcutSeconds(autoPageTurnSeconds), !automaticPageTurnActive);
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_HIGHLIGHT:
      openClippingSelection();
      return true;
    case CrossPointSettings::LP_MENU_SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_DISABLED:
      return false;
    default:
      return false;
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) applyBookmarkJump(std::get<ProgressChangeResult>(result.data));
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SEARCH_TEXT:
      if (!epub || !section || buildViewportWidth == 0 || buildViewportHeight == 0) {
        requestUpdate();
        break;
      }
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_IN_BOOK), "",
                                                                     BOOK_SEARCH_QUERY_BYTES, InputType::Text, true),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled || !epub || !section) {
                                 requestUpdate();
                                 return;
                               }
                               const std::string query = std::get<KeyboardResult>(result.data).text;
                               if (query.empty()) {
                                 requestUpdate();
                                 return;
                               }
                               EpubInBookSearchActivity::Layout layout;
                               layout.fontId = SETTINGS.getReaderFontId();
                               layout.lineCompression = SETTINGS.getReaderLineCompression();
                               layout.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
                               layout.paragraphAlignment = SETTINGS.paragraphAlignment;
                               layout.viewportWidth = buildViewportWidth;
                               layout.viewportHeight = buildViewportHeight;
                               layout.hyphenationEnabled = SETTINGS.hyphenationEnabled;
                               layout.embeddedStyle = activeEmbeddedStyle();
                               layout.imageRendering = SETTINGS.imageRendering;
                               layout.focusReadingEnabled = SETTINGS.focusReadingEnabled;
                               layout.wordSpacing = SETTINGS.wordSpacing;
                               layout.renderMode = activeEpubRenderMode();
                               layout.forceParagraphIndents = SETTINGS.forceParagraphIndents != 0;
                               const int searchSpine = currentSpineIndex;
                               const int searchPage = section->currentPage;
                               startActivityForResult(
                                   std::make_unique<EpubInBookSearchActivity>(renderer, mappedInput, epub, query,
                                                                              searchSpine, searchPage, layout),
                                   [this](const ActivityResult& searchResult) {
                                     if (!searchResult.isCancelled) {
                                       const auto* jump = std::get_if<ProgressChangeResult>(&searchResult.data);
                                       if (jump && jump->hasSavedProgress) {
                                         RenderLock lock(*this);
                                         currentSpineIndex = jump->spineIndex;
                                         nextPageNumber = jump->page;
                                         pendingPageJump = static_cast<uint16_t>(std::max(0, jump->page));
                                         section.reset();
                                       }
                                     }
                                     requestUpdate();
                                   });
                             });
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PAGE:
    case EpubReaderMenuActivity::MenuAction::MARK_COMPLETE:
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_SETTINGS: {
      openBookReaderSettings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      openReadingStats();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING: {
      openClippingSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      openClippings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SAVED_ITEMS: {
      openSavedItems();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          const int backupSpine = currentSpineIndex;
          const int backupPage = section->currentPage;
          // A partial section's pageCount is only its built watermark. Persist
          // the best total estimate before deleting the section cache so a
          // rebuild can restore the same relative position.
          const int backupPageCount = section->estimatedTotalPages();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
            pendingSyncSaveError = true;
            requestUpdate();
            break;
          }
          rememberCurrentContentOffset();
          cachedSpineIndex = backupSpine;
          nextPageNumber = backupPage;
          cachedChapterTotalPageCount = backupPageCount;
          section.reset();
          if (!clearBookCacheDirectoryPreservingUserState(epub->getCachePath())) {
            LOG_ERR("ERS", "Failed to clear derived book cache without risking user data");
            // A failed rollback may leave the only authoritative copy of some
            // per-book files in the sibling staging directory. Recover it now;
            // if that is still impossible, release the book and leave before
            // render()/onExit() can create conflicting state in the cache.
            if (!recoverBookCacheUserState(epub->getCachePath(), epub->getPath())) {
              LOG_ERR("ERS", "Cache state recovery remains incomplete; leaving reader fail-closed");
              bookSettingsWritable = false;
              bookReadingStatsWritable = false;
              ImageBlock::setExtractor(nullptr, nullptr);
              epub.reset();
              lock.unlock();
              onGoHome();
              return;
            }
            pendingCacheClearError = true;
            requestUpdate();
            break;
          }
          epub->setupCacheDir();
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC: {
      launchNearbyPositionSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      showBookmarkMessage = addBookmark();
      if (showBookmarkMessage) {
        bookmarkMessageTime = millis();
        requestUpdate();
      }
      break;
    }
  }
}

void EpubReaderActivity::applyBookmarkJump(const ProgressChangeResult& sync) {
  const bool contentJump =
      sync.hasContentSourceOffset && sync.spineIndex >= 0 && epub && sync.spineIndex < epub->getSpineItemsCount();
  int targetSpineIndex = sync.spineIndex;
  int targetPage = sync.page;
  int activeSpineIndex = 0;
  int activeTotalPages = 0;
  int fallbackTotalPages = 0;
  bool hasSection = false;
  {
    RenderLock lock(*this);
    activeSpineIndex = currentSpineIndex;
    hasSection = section != nullptr;
    activeTotalPages = section ? section->estimatedTotalPages() : 0;
    fallbackTotalPages = section ? activeTotalPages : cachedChapterTotalPageCount;
  }
  const bool cachedPageMatchesActiveSection = !contentJump && hasSection && sync.totalPages > 0 &&
                                              activeSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                              sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

  if (!contentJump && !cachedPageMatchesActiveSection && sync.hasSavedProgress) {
    const CrossPointPosition fallback = ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer,
                                                                     activeSpineIndex, fallbackTotalPages);
    targetSpineIndex = fallback.spineIndex;
    targetPage = fallback.pageNumber;
  }

  RenderLock lock(*this);
  pendingBookmarkSourceOffset = contentJump ? std::optional<uint32_t>(sync.contentSourceOffset) : std::nullopt;
  currentPageSourceOffset.reset();
  if (contentJump) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = std::max(0, targetPage);
    section.reset();
    return;
  }
  if (currentSpineIndex != targetSpineIndex) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = targetPage;
    section.reset();
  } else if (section && section->currentPage != targetPage) {
    section->currentPage = std::max(0, targetPage);
  } else if (!section) {
    nextPageNumber = targetPage;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch
  if (pendingReadFolderMove) {
    pendingFinishedMoveSyncError = true;
    requestUpdate();
    return true;
  }

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // The reader releases its Epub object before ActivityManager can run
  // onExit(), so persist the finished session while the cache path still exists.
  commitReadingSession();
  saveReadingStats();

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    // ImageBlock keeps a non-owning callback into Epub; clear it before the
    // early release, just as onExit() does.
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

bool EpubReaderActivity::launchNearbyPositionSync() {
  if (!epub) return false;
  if (pendingReadFolderMove) {
    pendingFinishedMoveSyncError = true;
    requestUpdate();
    return true;
  }

  const CrossPointPosition localPosition = getCurrentPosition();
  const SavedProgressPosition savedPosition = ProgressMapper::toSavedProgress(epub, localPosition);
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  const std::string chapterName = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : "";
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  // Nearby leaves through a radio-cleanup restart. Replace the reader (instead
  // of stacking a child) so onExit commits reading statistics and restores the
  // per-book settings overlay before any possible reboot.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }
  commitReadingSession();
  saveReadingStats();
  const NearbyReaderLayout nearbyLayout{
      SETTINGS.getReaderFontId(),
      SETTINGS.getReaderLineCompression(),
      SETTINGS.extraParagraphSpacing != 0,
      SETTINGS.paragraphAlignment,
      buildViewportWidth,
      buildViewportHeight,
      SETTINGS.hyphenationEnabled != 0,
      activeEmbeddedStyle(),
      SETTINGS.imageRendering,
      SETTINGS.focusReadingEnabled != 0,
      SETTINGS.wordSpacing,
      activeEpubRenderMode(),
      SETTINGS.forceParagraphIndents != 0,
  };
  activityManager.replaceActivity(std::make_unique<NearbyPositionSyncActivity>(
      renderer, mappedInput, epub, localPosition, savedPosition, chapterName, nearbyLayout));
  return true;
}

bool EpubReaderActivity::persistBookReaderSettings() {
  if (!epub || !bookSettingsWritable) return false;

  // Even a disabled record is useful: it prevents the immutable CrossInk file
  // from being imported again after the user explicitly chooses Reset or Off.
  return PerBookReaderSettingsStore::save(epub->getCachePath(), bookReaderSettings) ==
         PerBookReaderSettingsStore::SaveStatus::SAVED;
}

bool EpubReaderActivity::queueSafeModePromptIfEligible(const EpubBuildStatus status) {
  if (status != EpubBuildStatus::OutOfMemory || SETTINGS.epubSafeMode != 0) return false;
  safeModePromptRequested.store(true, std::memory_order_release);
  automaticPageTurnActive = false;
  return true;
}

void EpubReaderActivity::invalidateReaderLayout() {
  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  sdFontSystem.ensureLoaded(renderer, false);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  section.reset();
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedContentSourceOffset.reset();
  if (!section || section->currentPage < 0 || section->currentPage >= section->pageCount) return;
  if (const auto page = section->loadPage(section->currentPage)) {
    cachedContentSourceOffset = PageSourceAnchor::first(*page);
  }
}

void EpubReaderActivity::openBookReaderSettings() {
  if (!bookSettingsWritable) {
    pendingBookSettingsSaveError = true;
    requestUpdate();
    return;
  }

  const bool embeddedStylesWereEnabled = activeEmbeddedStyle();
  startActivityForResult(
      std::make_unique<BookReaderSettingsActivity>(renderer, mappedInput, globalReaderSettings, bookReaderSettings),
      [this, embeddedStylesWereEnabled](const ActivityResult& result) {
        if (result.isCancelled || !std::holds_alternative<ReaderSettingsResult>(result.data)) return;
        PerBookReaderSettings updated = std::get<ReaderSettingsResult>(result.data).settings;
        if (updated == bookReaderSettings) return;

        const PerBookReaderSettings previous = bookReaderSettings;
        const auto applyEffectiveSettings = [this](const PerBookReaderSettings& settings) {
          applyEffectiveBookReaderSettings(globalReaderSettings, settings);
        };
        const bool autoPageTurnChanged = updated.hasAutoPageTurnInterval != previous.hasAutoPageTurnInterval ||
                                         updated.autoPageTurnSeconds != previous.autoPageTurnSeconds ||
                                         updated.autoPageTurnStartsOnOpen != previous.autoPageTurnStartsOnOpen;
        const PerBookReaderSettings& updatedTypography = updated.hasReaderOverrides ? updated : globalReaderSettings;
        const bool embeddedStylesWillBeEnabled = !updated.safeModeEnabled && updatedTypography.embeddedStyle != 0;

        // A book opened with Embedded Styles disabled intentionally has no
        // external CSS cache. Prepare and verify it before committing an
        // OFF -> ON change. Drop the live Section first so its files are
        // closed and its exact page remains the rollback/reflow anchor.
        if (!embeddedStylesWereEnabled && embeddedStylesWillBeEnabled) {
          applyEffectiveSettings(previous);
          invalidateReaderLayout();

          bool cssReady = false;
          if (epub) {
            GfxRenderer::FrameBufferLoan loan(renderer);
            cssReady = epub->ensureCssCache();
          }
          if (!cssReady) {
            bookReaderSettings = previous;
            applyEffectiveSettings(previous);
            pendingBookStylesApplyError = true;
            requestUpdate();
            return;
          }
        }

        bookReaderSettings = updated;
        applyEffectiveSettings(bookReaderSettings);
        if (autoPageTurnChanged) {
          applyAutoPageTurnRuntime(
              bookReaderSettings.hasAutoPageTurnInterval ? bookReaderSettings.autoPageTurnSeconds : 0,
              bookReaderSettings.autoPageTurnStartsOnOpen);
        }

        // The store publishes atomically. If publication fails, keep runtime
        // and disk on the same previous profile instead of applying a setting
        // that silently disappears on the next open.
        if (!persistBookReaderSettings()) {
          bookReaderSettings = previous;
          applyEffectiveSettings(previous);
          if (autoPageTurnChanged) {
            applyAutoPageTurnRuntime(previous.hasAutoPageTurnInterval ? previous.autoPageTurnSeconds : 0,
                                     previous.autoPageTurnStartsOnOpen);
          }
          pendingBookSettingsSaveError = true;
        }
        invalidateReaderLayout();
        requestUpdate();
      });
}

void EpubReaderActivity::openReadingStats() {
  if (!epub) return;

  // Opening the statistics screen is a real visibility boundary. Consume the
  // current page interval now so the numbers on the screen include it; the
  // redraw after returning starts a fresh interval.
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  const bool hasFreshTimeEstimate = refreshEstimatedTimeLeft();
  const bool previewCurrentSession = !readingSessionCommitted;
  BookReadingStats displayBookStats = bookReadingStats;
  GlobalReadingStats displayDeviceStats = globalReadingStats;
  if (previewCurrentSession) {
    previewReadingStatsSession(bookReadingStatsWritable ? &displayBookStats : nullptr,
                               globalReadingStatsWritable ? &displayDeviceStats : nullptr, sessionReadingSeconds,
                               pendingBookReadingSpans, pendingGlobalReadingSpans,
                               hasSessionStartLocalDateTime ? &sessionStartLocalDateTime : nullptr);
  }

  const GlobalReadingStatsAggregation allSyncedStats = GlobalReadingStats::loadAggregatedWithReport(displayDeviceStats);

  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  if (bookReadingStatsTrusted && bookReadingStats.isCompleted) {
    progress = ReadingStatsMetric::known(100);
  } else if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage + 1) / static_cast<float>(section->estimatedTotalPages());
    const int percent =
        clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
    progress = ReadingStatsMetric::estimated(static_cast<uint32_t>(percent));
  }

  ReadingStatsDateTime now;
  const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  ReadingStatsPresentation presentation = buildReadingStatsPresentation(
      displayBookStats, bookReadingStatsTrusted, displayDeviceStats, globalReadingStatsTrusted, allSyncedStats,
      currentDateTime, progress, hasFreshTimeEstimate);
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, epub->getTitle(), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, bookReadingStatsWritable, false),
      [this](const ActivityResult& result) {
        const auto* action = std::get_if<ReadingStatsActionResult>(&result.data);
        if (!action || action->action != ReadingStatsActionResult::Action::EditBookDates || !epub ||
            !bookReadingStatsWritable) {
          return;
        }
        const std::string cachePath = epub->getCachePath();
        startActivityForResult(
            std::make_unique<ReadingStatsDateEditActivity>(renderer, mappedInput, cachePath, bookReadingStats),
            [this, cachePath](const ActivityResult& editResult) {
              if (editResult.isCancelled) return;
              BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
              bookReadingStats = BookReadingStats::load(cachePath, &status);
              bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(status);
              bookReadingStatsWritable = bookReadingStatsTrusted && BookReadingStats::canPublish(cachePath);
              bookReadingStatsDirty = false;
              requestUpdate();
            });
      });
}

void EpubReaderActivity::openClippingSelection() {
  if (!epub || !section || !clippingStore.isLoaded()) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }
  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    pendingClippingNotice = ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) {
    pendingClippingNotice = ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
  // Multi-page clipping may extend only through pages that are already built.
  // A partial/estimated count is not presented as a loadable page range.
  const uint16_t pageCount = section->pageCount;
  const uint16_t paragraphIndex = section->getParagraphIndexForPage(currentPage).value_or(UINT16_MAX);
  const ClipSelectionActivity::PageLoader pageLoader{
      this, [](void* context, const uint16_t targetPage) -> std::unique_ptr<Page> {
        auto* reader = static_cast<EpubReaderActivity*>(context);
        if (!reader->section || targetPage >= reader->section->pageCount) return {};
        return reader->section->loadPage(targetPage);
      }};

  startActivityForResult(std::make_unique<ClipSelectionActivity>(
                             renderer, mappedInput, std::move(page), SETTINGS.getReaderFontId(), marginLeft, marginTop,
                             currentPage, pageCount, paragraphIndex, currentClippingLayoutFingerprint(), pageLoader,
                             &clippingStore.entries(), static_cast<uint16_t>(currentSpineIndex)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* selection = std::get_if<ClippingSelectionResult>(&result.data);
                           if (!selection || !epub || !clippingStore.isLoaded()) {
                             pendingClippingNotice = ClippingNotice::Unavailable;
                             requestUpdate();
                             return;
                           }

                           ClippingCodec::ClippingMetadata clipping;
                           clipping.spineIndex = static_cast<uint16_t>(currentSpineIndex);
                           clipping.startPage = selection->startPage;
                           clipping.endPage = selection->endPage;
                           clipping.pageCount = selection->pageCount;
                           // These are stable indexes among all visible tokens on the page. The
                           // UI-vector indexes are intentionally not persisted.
                           clipping.startWordIndex = selection->startPageWordIndex;
                           clipping.endWordIndex = selection->endPageWordIndex;
                           clipping.wordCount = selection->wordCount;
                           clipping.paragraphIndex = selection->paragraphIndex;
                           clipping.pageFingerprint = selection->pageFingerprint;
                           clipping.layoutFingerprint = selection->layoutFingerprint;
                           clipping.hasTextAnchor = selection->hasTextAnchor;
                           clipping.textSourceStart = selection->textSourceStart;
                           clipping.textSourceEnd = selection->textSourceEnd;
                           const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
                           if (tocIndex >= 0) clipping.chapterTitle = epub->getTocItem(tocIndex).title;
                           const std::time_t now = std::time(nullptr);
                           if (now >= 1577836800 && static_cast<uint64_t>(now) <= UINT32_MAX) {
                             clipping.timestamp = static_cast<uint32_t>(now);
                           }

                           switch (clippingStore.add(clipping, selection->text)) {
                             case ClippingStore::AddResult::Added:
                               pendingClippingNotice = ClippingNotice::Saved;
                               break;
                             case ClippingStore::AddResult::LimitReached:
                               pendingClippingNotice = ClippingNotice::LimitReached;
                               break;
                             case ClippingStore::AddResult::InvalidData:
                             case ClippingStore::AddResult::SaveFailed:
                               pendingClippingNotice = ClippingNotice::SaveFailed;
                               break;
                           }
                           requestUpdate();
                         });
}

uint32_t EpubReaderActivity::currentClippingLayoutFingerprint() const {
  ClippingPageTools::LayoutIdentity identity;
  identity.fontId = SETTINGS.getReaderFontId();
  identity.lineCompression = SETTINGS.getReaderLineCompression();
  identity.extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0;
  identity.paragraphAlignment = SETTINGS.paragraphAlignment;
  identity.viewportWidth = buildViewportWidth;
  identity.viewportHeight = buildViewportHeight;
  identity.hyphenationEnabled = SETTINGS.hyphenationEnabled != 0;
  identity.embeddedStyle = activeEmbeddedStyle();
  identity.imageRendering = SETTINGS.imageRendering;
  identity.focusReadingEnabled = SETTINGS.focusReadingEnabled != 0;
  identity.wordSpacing = SETTINGS.wordSpacing;
  identity.renderMode = static_cast<uint8_t>(activeEpubRenderMode());
  identity.forceParagraphIndents = SETTINGS.forceParagraphIndents != 0;
  return ClippingPageTools::layoutFingerprint(identity);
}

void EpubReaderActivity::openClippings() {
  if (!epub || !clippingStore.isLoaded()) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<ClippingListActivity>(renderer, mappedInput, clippingStore),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* jump = std::get_if<ClippingJumpResult>(&result.data);
                           if (!jump || !validateClippingJump(*jump)) {
                             RenderLock lock(*this);
                             pendingClippingNotice = ClippingNotice::JumpUnavailable;
                             requestUpdate();
                             return;
                           }

                           {
                             RenderLock lock(*this);
                             armClippingJump(*jump);
                           }
                           requestUpdate();
                         });
}

void EpubReaderActivity::openSavedItems() {
  if (!epub) return;
  startActivityForResult(std::make_unique<BookSavedItemsActivity>(renderer, mappedInput, epub, &clippingStore),
                         [this](const ActivityResult& result) {
                           loadCachedBookmarks();
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           if (const auto* progress = std::get_if<ProgressChangeResult>(&result.data)) {
                             applyBookmarkJump(*progress);
                             requestUpdate();
                             return;
                           }
                           const auto* jump = std::get_if<ClippingJumpResult>(&result.data);
                           if (!jump || !validateClippingJump(*jump)) {
                             RenderLock lock(*this);
                             pendingClippingNotice = ClippingNotice::JumpUnavailable;
                             requestUpdate();
                             return;
                           }
                           {
                             RenderLock lock(*this);
                             armClippingJump(*jump);
                           }
                           requestUpdate();
                         });
}

bool EpubReaderActivity::validateClippingJump(const ClippingJumpResult& jump) const {
  if (!epub || !clippingStore.isLoaded() || jump.clippingIndex >= clippingStore.size() ||
      jump.bookPath != epub->getPath() || jump.bookType != "epub") {
    return false;
  }
  const std::string canonicalStore = ClippingCodec::filePathForBook(jump.bookPath, jump.bookType);
  if (canonicalStore.empty() || canonicalStore != jump.storePath || canonicalStore != clippingStore.path()) {
    return false;
  }

  std::string text;
  if (!clippingStore.readText(jump.clippingIndex, text)) return false;
  const uint32_t textCrc =
      ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(text.data()), static_cast<size_t>(text.size()));
  const ClippingJumpValidation::StoreSnapshot snapshot{clippingStore.book(),
                                                       clippingStore.path(),
                                                       clippingStore.format(),
                                                       clippingStore.fileLength(),
                                                       clippingStore.entries(),
                                                       epub->getSpineItemsCount(),
                                                       textCrc};
  return ClippingJumpValidation::isExact(jump, snapshot);
}

void EpubReaderActivity::armClippingJump(const ClippingJumpResult& jump) {
  const int fallbackPage = section ? section->currentPage : nextPageNumber;
  const bool keepSection = section && currentSpineIndex == jump.spineIndex;
  pendingClippingJump = PendingClippingJump{
      jump.clippingIndex,
      jump.spineIndex,
      jump.startPage,
      jump.pageCount,
      jump.paragraphIndex,
      jump.pageFingerprint,
      jump.layoutFingerprint,
      0,
      0,
      currentSpineIndex,
      fallbackPage,
      cachedSpineIndex,
      cachedChapterTotalPageCount,
  };

  // Prevent a cached-progress reflow adjustment from moving the exact target
  // before its fingerprint is checked.
  cachedSpineIndex = jump.spineIndex;
  cachedChapterTotalPageCount = 0;
  cachedContentSourceOffset.reset();
  currentSpineIndex = jump.spineIndex;
  const bool layoutKnown = buildViewportWidth > 0 && buildViewportHeight > 0;
  const bool layoutChanged =
      layoutKnown && jump.layoutFingerprint != 0 && jump.layoutFingerprint != currentClippingLayoutFingerprint();
  nextPageNumber = layoutChanged ? 0 : jump.startPage;
  if (keepSection) {
    section->currentPage = nextPageNumber;
  } else {
    section.reset();
  }
}

bool EpubReaderActivity::abortPendingClippingJump(const bool showNotice) {
  if (!pendingClippingJump) return false;
  const PendingClippingJump jump = *pendingClippingJump;
  currentSpineIndex = jump.fallbackSpineIndex;
  nextPageNumber = jump.fallbackPage;
  cachedSpineIndex = jump.fallbackCachedSpineIndex;
  cachedChapterTotalPageCount = jump.fallbackCachedChapterPageCount;
  section.reset();
  pendingClippingJump.reset();
  pendingClippingReanchorLaunch.store(false, std::memory_order_release);
  if (showNotice) pendingClippingNotice = ClippingNotice::JumpUnavailable;
  requestUpdate();
  return true;
}

bool EpubReaderActivity::preparePendingClippingJump() {
  if (!pendingClippingJump) return false;
  if (!section || currentSpineIndex != pendingClippingJump->spineIndex) return abortPendingClippingJump();

  PendingClippingJump& jump = *pendingClippingJump;
  const uint32_t currentLayout = currentClippingLayoutFingerprint();
  if (jump.layoutFingerprint != 0 && jump.layoutFingerprint != currentLayout) {
    if (section->pageCount == 0) return abortPendingClippingJump();
    const std::optional<uint16_t> paragraphPage =
        jump.paragraphIndex == UINT16_MAX ? std::nullopt : section->getPageForParagraphIndex(jump.paragraphIndex);
    std::optional<uint16_t> proportionalPage;
    if (jump.pageCount > 1) {
      const uint32_t newTotal = std::max<uint16_t>(section->estimatedTotalPages(), section->pageCount);
      proportionalPage = static_cast<uint16_t>(
          (static_cast<uint32_t>(jump.page) * (newTotal - 1) + (jump.pageCount - 1) / 2) / (jump.pageCount - 1));
    }

    // A paragraph can span several pages.  Its LUT entry points to the first
    // page of that paragraph, which may be far ahead of the actual highlight.
    // When the paragraph and old-page estimates fit in the nine-page budget,
    // scan the complete interval; otherwise prefer the proportional estimate
    // for long paragraphs.  The matcher remains exact and still rejects zero
    // or multiple matches, so this only improves the candidate window.
    uint16_t expectedPage = paragraphPage.value_or(proportionalPage.value_or(0));
    uint16_t searchFirstPage = 0;
    uint16_t searchLastPage = 0;
    if (paragraphPage && proportionalPage &&
        std::max(*paragraphPage, *proportionalPage) - std::min(*paragraphPage, *proportionalPage) <
            ClippingPageTools::MAX_REANCHOR_PAGES) {
      searchFirstPage = std::min(*paragraphPage, *proportionalPage);
      searchLastPage = std::max(*paragraphPage, *proportionalPage);
      expectedPage = static_cast<uint16_t>((searchFirstPage + searchLastPage) / 2);
    } else if (proportionalPage) {
      expectedPage = *proportionalPage;
    }
    const uint16_t estimatedTotal = std::max<uint16_t>(section->estimatedTotalPages(), section->pageCount);
    expectedPage = std::min<uint16_t>(expectedPage, static_cast<uint16_t>(estimatedTotal - 1));
    section->currentPage = expectedPage;
    if (expectedPage >= section->pageCount) {
      requestUpdate();
      return true;
    }

    if (searchLastPage == 0 && searchFirstPage == 0) {
      constexpr uint16_t SEARCH_RADIUS = 4;
      searchFirstPage = expectedPage > SEARCH_RADIUS ? static_cast<uint16_t>(expectedPage - SEARCH_RADIUS) : 0;
      searchLastPage = static_cast<uint16_t>(std::min<uint32_t>(static_cast<uint32_t>(section->pageCount - 1),
                                                                static_cast<uint32_t>(expectedPage) + SEARCH_RADIUS));
    }
    jump.searchFirstPage = searchFirstPage;
    const uint16_t searchLimit = section->isBuilding()
                                     ? static_cast<uint16_t>(std::max<uint16_t>(section->estimatedTotalPages(), 1) - 1)
                                     : static_cast<uint16_t>(section->pageCount - 1);
    // Keep the interval valid even when the proportional estimate points
    // beyond the currently known/buildable section.  Reanchor activity is
    // deliberately fail-closed for an invalid range, so clamp both ends
    // before launching it.
    jump.searchFirstPage = std::min<uint16_t>(searchFirstPage, searchLimit);
    jump.searchLastPage = std::max<uint16_t>(jump.searchFirstPage, std::min<uint16_t>(searchLastPage, searchLimit));
    // A partial section exposes only the pages laid out by the active build.
    // The matcher must not start on a page past that watermark: loadPage()
    // would fail and turn an otherwise valid highlight into a false
    // "position could not be determined" error.  Drive the cooperative build
    // through the end of the bounded search window before launching it.
    if (section->isBuilding() && section->pageCount <= jump.searchLastPage) {
      section->currentPage = jump.searchLastPage;
    }
    pendingClippingReanchorLaunch.store(true, std::memory_order_release);
    return true;
  }

  if (jump.pageFingerprint == 0 || jump.page >= section->pageCount) return abortPendingClippingJump();

  section->currentPage = jump.page;
  return false;
}

void EpubReaderActivity::launchPendingClippingReanchor() {
  RenderLock lock(*this);
  if (!pendingClippingJump || !section || currentSpineIndex != pendingClippingJump->spineIndex) {
    abortPendingClippingJump();
    return;
  }
  const PendingClippingJump jump = *pendingClippingJump;
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  startActivityForResult(
      std::make_unique<ClippingReanchorActivity>(
          renderer, mappedInput, *section, clippingStore, jump.clippingIndex, jump.searchFirstPage, jump.searchLastPage,
          currentClippingLayoutFingerprint(), SETTINGS.getReaderFontId(), marginLeft, marginTop),
      [this](const ActivityResult& result) {
        const auto* page = std::get_if<PageResult>(&result.data);
        bool reanchorApplied = false;
        const bool reanchorFailed = !result.isCancelled;
        {
          RenderLock lock(*this);
          if (page && pendingClippingJump && section && page->page < section->pageCount) {
            section->currentPage = static_cast<int>(page->page);
            pendingClippingJump.reset();
            reanchorApplied = true;
          } else {
            abortPendingClippingJump(false);
            if (reanchorFailed) pendingClippingNotice = ClippingNotice::ReanchorFailed;
          }
        }
        if (reanchorApplied || reanchorFailed) requestUpdate();
      });
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // This singleton is the active per-book overlay. Persist it in the book's
    // own file, never in the global settings.json.
    SETTINGS.orientation = orientation;
    bookReaderSettings =
        captureReaderSettings(true, bookReaderSettings.hasAutoPageTurnInterval, bookReaderSettings.autoPageTurnSeconds,
                              bookReaderSettings.autoPageTurnStartsOnOpen);

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
  if (!persistBookReaderSettings()) pendingBookSettingsSaveError = true;
}

void EpubReaderActivity::applyAutoPageTurnRuntime(const uint8_t seconds, const bool active) {
  const bool wasActive = automaticPageTurnActive;
  autoPageTurnSeconds = normalizeAutoPageTurnSeconds(seconds);
  automaticPageTurnActive = active && autoPageTurnSeconds != 0;
  pageTurnDuration =
      automaticPageTurnActive ? static_cast<unsigned long>(autoPageTurnSeconds) * MILLISECONDS_PER_SECOND : 0UL;
  if (automaticPageTurnActive) lastPageTurnTime = millis();

  if (wasActive == automaticPageTurnActive) return;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // Reflow only when the indicator is shown or hidden; changing the interval
  // does not alter the reserved status-bar space.
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::updateAutoPageTurnFromMenu(const uint8_t seconds) {
  const uint8_t normalizedSeconds = normalizeAutoPageTurnSeconds(seconds);
  const bool settingsChanged = bookReaderSettings.hasAutoPageTurnInterval != (normalizedSeconds != 0) ||
                               bookReaderSettings.autoPageTurnSeconds != normalizedSeconds ||
                               bookReaderSettings.autoPageTurnStartsOnOpen != (normalizedSeconds != 0);

  bookReaderSettings.hasAutoPageTurnInterval = normalizedSeconds != 0;
  bookReaderSettings.autoPageTurnSeconds = normalizedSeconds;
  bookReaderSettings.autoPageTurnStartsOnOpen = normalizedSeconds != 0;
  applyAutoPageTurnRuntime(normalizedSeconds, normalizedSeconds != 0);

  if (settingsChanged && !persistBookReaderSettings()) pendingBookSettingsSaveError = true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  consumeReadingViewSignal();
  stopReadingPage(isForwardTurn, static_cast<uint32_t>(millis()));
  {
    RenderLock lock(*this);
    coverSkipDirection = isForwardTurn ? CoverSkipDirection::Forward : CoverSkipDirection::Backward;
    coverSkipHops = 0;

    if (section && isForwardTurn) {
      // Advance within the section while there are (or may still be) more pages: either a built
      // page ahead, or the section is still building/partial (windowed), in which case more pages exist
      // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
      // the section is fully built AND we're on its last page do we move to the next spine -- using
      // the live pageCount alone would mistake the build watermark for the end of a giant spine.
      if (section->currentPage < section->pageCount - 1 || section->isBuilding() || section->isPartial()) {
        section->currentPage++;
      } else {
        nextPageNumber = 0;
        currentSpineIndex++;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        debugBeginSectionOpen(true);
#endif
        section.reset();
      }
    } else if (section) {
      if (section->currentPage > 0) {
        section->currentPage--;
      } else if (currentSpineIndex > 0) {
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        debugBeginSectionOpen(true);
#endif
        section.reset();
      }
    }
    refreshEstimatedTimeLeft();
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

bool EpubReaderActivity::moveOnePageWithoutRendering(const bool forward) {
  if (!section) return false;

  if (forward) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding() || section->isPartial()) {
      ++section->currentPage;
      return true;
    }
    if (currentSpineIndex + 1 >= epub->getSpineItemsCount()) return false;

    nextPageNumber = 0;
    ++currentSpineIndex;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    debugBeginSectionOpen(true);
#endif
    section.reset();
    return true;
  }

  if (section->currentPage > 0) {
    --section->currentPage;
    return true;
  }
  if (currentSpineIndex <= 0) return false;

  nextPageNumber = 0;
  pendingPageJump = std::numeric_limits<uint16_t>::max();
  --currentSpineIndex;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  debugBeginSectionOpen(true);
#endif
  section.reset();
  return true;
}

bool EpubReaderActivity::skipCoverPageIfNeeded(const Page& page) {
  const bool skipEnabled = SETTINGS.skipEpubCoverPage != 0;
  const bool coverOnly = epub && page.isCoverOnly(epub->getCoverItemHref());
  // A number of EPUBs put the same cover artwork in an XHTML title page or in
  // the first content spine under a different image href. Once the exact OPF
  // href is unavailable, the safe bounded fallback is the first image-only
  // page of the opening spine(s). Do not classify later chapter illustrations
  // as covers.
  const bool leadingImageOnly = page.isImageOnly() && currentSpineIndex <= 1 && section && section->currentPage == 0;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (epub && page.hasImages()) {
    LOG_DBG("ERS", "Cover check: enabled=%u cover_only=%u leading_image_only=%u spine=%d page=%d href=%s",
            skipEnabled ? 1U : 0U, coverOnly ? 1U : 0U, leadingImageOnly ? 1U : 0U, currentSpineIndex,
            section ? section->currentPage : nextPageNumber, epub->getCoverItemHref().c_str());
  }
#endif
  if (!skipEnabled || !epub || (!coverOnly && !leadingImageOnly)) {
    coverSkipHops = 0;
    return false;
  }

  if (coverSkipHops >= MAX_COVER_SKIP_HOPS) {
    LOG_ERR("ERS", "Cover skip limit reached; showing the current page");
    coverSkipHops = 0;
    return false;
  }
  ++coverSkipHops;

  const bool forward = coverSkipDirection == CoverSkipDirection::Forward;
  bool moved = moveOnePageWithoutRendering(forward);
  if (!moved) {
    // A cover at a book boundary has no page in the preferred direction. Try
    // the nearest content page in the other direction before failing open.
    moved = moveOnePageWithoutRendering(!forward);
    if (moved) coverSkipDirection = forward ? CoverSkipDirection::Backward : CoverSkipDirection::Forward;
  }
  if (!moved) {
    coverSkipHops = 0;
    return false;
  }

  LOG_DBG("ERS", "Skipping EPUB cover page at spine %d page %d", currentSpineIndex,
          section ? section->currentPage : nextPageNumber);
  requestUpdate();
  return true;
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (renderReaderExitOverlay()) return;
  if (!epub) {
    signalReadingPageHidden();
    return;
  }

  bool clippingHighlightsTruncated = false;
  uint32_t renderedPageFingerprint = 0;
  const auto showPendingSyncSaveError = [this, &clippingHighlightsTruncated, &renderedPageFingerprint]() {
    if (clippingHighlightsTruncated && section &&
        clippingHighlightNotices.markIfNew(static_cast<uint16_t>(currentSpineIndex),
                                           static_cast<uint16_t>(section->currentPage), renderedPageFingerprint)) {
      pendingClippingHighlightsTruncatedNotice = true;
    }

    bool showedHighlightLimit = false;
    if (pendingFinishedMoveSyncError) {
      pendingFinishedMoveSyncError = false;
      GUI.drawPopup(renderer, tr(STR_SYNC_AFTER_FINISHED_MOVE));
    } else if (pendingSyncSaveError) {
      pendingSyncSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    } else if (pendingKOReaderCredentialsNotice) {
      pendingKOReaderCredentialsNotice = false;
      GUI.drawPopup(renderer, tr(STR_SET_CREDENTIALS_FIRST));
    } else if (pendingBookSettingsSaveError) {
      pendingBookSettingsSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_BOOK_SETTINGS_FAILED));
    } else if (pendingBookStylesApplyError) {
      pendingBookStylesApplyError = false;
      GUI.drawPopup(renderer, tr(STR_APPLY_BOOK_STYLES_FAILED));
    } else if (pendingExternalCssWarning) {
      if (externalCssWarningTime == 0) externalCssWarningTime = millis();
      GUI.drawPopup(renderer, tr(STR_PUBLISHER_STYLES_UNAVAILABLE));
    } else if (pendingCacheClearError) {
      pendingCacheClearError = false;
      GUI.drawPopup(renderer, tr(STR_CLEAR_CACHE_FAILED));
    } else if (pendingSafeModeEnabledNotice) {
      pendingSafeModeEnabledNotice = false;
      GUI.drawPopup(renderer, tr(STR_EPUB_SAFE_MODE_ENABLED));
    } else if (pendingStatsCompletionError) {
      pendingStatsCompletionError = false;
      GUI.drawPopup(renderer, tr(STR_COMPLETE_BOOK_STATS_FAILED));
    } else if (pendingBookmarkStorageError) {
      pendingBookmarkStorageError = false;
      GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
    } else if (pendingClippingNotice != ClippingNotice::None) {
      const ClippingNotice notice = pendingClippingNotice;
      pendingClippingNotice = ClippingNotice::None;
      switch (notice) {
        case ClippingNotice::Saved:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_SAVED));
          break;
        case ClippingNotice::LimitReached:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_LIMIT_REACHED));
          break;
        case ClippingNotice::SaveFailed:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_SAVE_FAILED));
          break;
        case ClippingNotice::NewerFormat:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_NEWER_FORMAT));
          break;
        case ClippingNotice::JumpUnavailable:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_JUMP_UNAVAILABLE));
          break;
        case ClippingNotice::ReanchorFailed:
          GUI.drawPopup(renderer, tr(STR_REANCHOR_UNIQUE_FAIL));
          break;
        case ClippingNotice::Unavailable:
          GUI.drawPopup(renderer, tr(STR_CLIPPING_UNAVAILABLE));
          break;
        case ClippingNotice::None:
          break;
      }
    } else if (pendingClippingHighlightsTruncatedNotice) {
      pendingClippingHighlightsTruncatedNotice = false;
      showedHighlightLimit = true;
      GUI.drawPopup(renderer, tr(STR_CLIPPING_HIGHLIGHTS_TRUNCATED));
    }

    // A higher-priority popup must delay, not discard, the memory-limit
    // warning. Render once more so every queued user-visible notice is shown.
    if (pendingClippingHighlightsTruncatedNotice && !showedHighlightLimit) requestUpdate();
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this](const bool safeModeFailed) {
    signalReadingPageHidden();
    if (abortPendingClippingJump()) return;
    renderer.clearScreen();
    GUI.drawPopup(renderer, safeModeFailed ? tr(STR_EPUB_SAFE_MODE_FAILED) : tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };
  const auto handleBuildFailure = [this, &showBuildError](const EpubBuildStatus failure) {
    section.reset();
    if (queueSafeModePromptIfEligible(failure)) {
      signalReadingPageHidden();
      return;
    }
    pendingSafeModePersistence.store(false, std::memory_order_release);
    showBuildError(failure == EpubBuildStatus::OutOfMemory && SETTINGS.epubSafeMode != 0);
  };

  if (pendingSafeModeFailureNotice.exchange(false, std::memory_order_acq_rel)) {
    showBuildError(false);
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    signalReadingPageHidden();
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  if (!section) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    // A chapter boundary arms this before the old Section is released, so its
    // suspension cost is included. Other open/reflow paths begin timing here.
    debugBeginSectionOpen(false);
#endif
    // An explicit navigation request always outranks a settings-change anchor
    // left over from the page that was previously visible.
    if (pendingPageJump.has_value() || !pendingAnchor.empty() || pendingPercentJump || pendingClippingJump) {
      cachedChapterTotalPageCount = 0;
      cachedContentSourceOffset.reset();
    }

    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t sectionCacheStartedMs = static_cast<uint32_t>(millis());
#endif
    const bool cacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, activeEmbeddedStyle(),
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing, activeEpubRenderMode(),
        SETTINGS.forceParagraphIndents != 0);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    LOG_DBG("ERTM", "section_cache_load spine=%d elapsed_ms=%u loaded=%u partial=%u pages=%u", currentSpineIndex,
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - sectionCacheStartedMs), cacheLoaded ? 1U : 0U,
            section->isPartial() ? 1U : 0U, static_cast<unsigned>(section->pageCount));
#endif
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
      cachedContentSourceOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (cacheLoaded && pendingBookmarkSourceOffset.has_value()) {
      const uint16_t hint = static_cast<uint16_t>(std::clamp(nextPageNumber, 0, static_cast<int>(UINT16_MAX)));
      if (const auto page = section->findPageForSourceOffset(*pendingBookmarkSourceOffset, hint)) {
        nextPageNumber = *page;
        pendingBookmarkSourceOffset.reset();
      } else if (cacheComplete) {
        // A malformed/stale anchor must not make the book unavailable. Keep
        // the legacy page hint as the deterministic fallback.
        pendingBookmarkSourceOffset.reset();
      }
    }
    const bool contentReposition =
        !cacheComplete && ((cachedContentSourceOffset.has_value() && currentSpineIndex == cachedSpineIndex) ||
                           pendingBookmarkSourceOffset.has_value());
    if (contentReposition) {
      section->setSourceOffsetTarget(pendingBookmarkSourceOffset.has_value() ? *pendingBookmarkSourceOffset
                                                                             : *cachedContentSourceOffset);
    }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    debugSetSectionCacheStatus(
        cacheComplete ? DebugSectionCacheStatus::Hit
                      : (section->isPartial() ? DebugSectionCacheStatus::Partial : DebugSectionCacheStatus::Miss));
#endif
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Only a percent jump needs the whole chapter up front (percent -> page needs the final page
      // count). Explicit pages and fragment anchors build incrementally until their landing page is
      // available. A settings-change reposition follows the same bounded path, stopping as soon as
      // the old page's canonical text offset appears in the new pagination; image-only pages retain
      // the proportional fallback after the final page count is known.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        renderer.clearScreen();
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        const uint16_t debugPagesBefore = section->debugBuiltPageCount();
#endif
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, activeEmbeddedStyle(),
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing,
                                        activeEpubRenderMode(),
                                        SETTINGS.forceParagraphIndents != 0, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          const EpubBuildStatus failure = section->lastBuildStatus();
          loan.end();  // restore before anything draws
          handleBuildFailure(failure);
          return;
        }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        debugRecordSectionBuild(debugPagesBefore, false);
#endif
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() && !contentReposition &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            renderer.clearScreen();
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Lend the framebuffer's 48 KB to the blocking pre-render burst
          // (startBuild inflates the whole spine HTML — the memory peak). The
          // background buildSomeMore chunks in loop() do NOT get the loan: they
          // deliberately interleave with page renders. Restored before render.
          GfxRenderer::FrameBufferLoan loan(renderer);
          if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                   SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                   viewportHeight, SETTINGS.hyphenationEnabled, activeEmbeddedStyle(),
                                   SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing,
                                   activeEpubRenderMode(),
                                   SETTINGS.forceParagraphIndents != 0)) {
            LOG_ERR("ERS", "Failed to start section build");
            const EpubBuildStatus failure = section->lastBuildStatus();
            loan.end();  // restore before anything draws (showBuildError renders a popup)
            handleBuildFailure(failure);
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump          ? !section->findAnchor(pendingAnchor)
                  : contentReposition ? !section->sourceOffsetTargetPage().has_value()
                                      : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
            const uint16_t debugPagesBefore = section->debugBuiltPageCount();
#endif
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              const EpubBuildStatus failure = section->lastBuildStatus();
              loan.end();  // restore before anything draws (showBuildError renders a popup)
              handleBuildFailure(failure);
              return;
            }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
            debugRecordSectionBuild(debugPagesBefore, false);
#endif
          }
          loan.end();
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (pendingBookmarkSourceOffset.has_value()) {
      if (const auto contentPage = section->sourceOffsetTargetPage()) section->currentPage = *contentPage;
      pendingBookmarkSourceOffset.reset();
      section->clearSourceOffsetTarget();
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() &&
        !section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                             SETTINGS.hyphenationEnabled, activeEmbeddedStyle(), SETTINGS.imageRendering,
                             SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing, activeEpubRenderMode(),
                             SETTINGS.forceParagraphIndents != 0)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      const EpubBuildStatus failure = section->lastBuildStatus();
      handleBuildFailure(failure);
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      const uint16_t debugPagesBefore = section->debugBuiltPageCount();
#endif
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        const EpubBuildStatus failure = section->lastBuildStatus();
        handleBuildFailure(failure);
        return;
      }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      debugRecordSectionBuild(debugPagesBefore, false);
#endif
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      const uint16_t debugPagesBefore = section->debugBuiltPageCount();
#endif
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        const EpubBuildStatus failure = section->lastBuildStatus();
        handleBuildFailure(failure);
        return;
      }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      debugRecordSectionBuild(debugPagesBefore, false);
#endif
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();
  if (preparePendingClippingJump()) {
    signalReadingPageHidden();
    return;
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    signalReadingPageHidden();
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    signalReadingPageHidden();
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  currentPageSourceOffset.reset();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t pageLoadStartedMs = static_cast<uint32_t>(millis());
#endif
    auto p = section->loadPage(section->currentPage);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    LOG_DBG("ERTM", "page_load spine=%d page=%d elapsed_ms=%u ok=%u", currentSpineIndex, section->currentPage,
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - pageLoadStartedMs), p ? 1U : 0U);
#endif
    if (!p) {
      // A clipping jump is a read-only, exact operation. Do not rebuild or
      // clear its target cache after a failed read: restore the original
      // position and let normal navigation diagnose/rebuild that cache later.
      if (pendingClippingJump) {
        LOG_ERR("ERS", "Failed to load exact clipping target - restoring previous position");
        automaticPageTurnActive = false;
        abortPendingClippingJump();
        signalReadingPageHidden();
        showPendingSyncSaveError();
        return;
      }

      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        signalReadingPageHidden();
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly
    currentPageSourceOffset = PageSourceAnchor::first(*p);
    updateBookmarkFlag();

    // Verify the very same Page object that will be rendered. Loading once to
    // validate and again to render would leave a TOCTOU window where an SD
    // failure or changed cache could save an unverified target as progress.
    if (pendingClippingJump) {
      PendingClippingJump& jump = *pendingClippingJump;
      const bool exactTarget =
          currentSpineIndex == jump.spineIndex && section->currentPage == jump.page &&
          ClippingPageTools::fingerprint(*p, renderer, SETTINGS.getReaderFontId(), orientedMarginLeft,
                                         orientedMarginTop) == jump.pageFingerprint;
      if (!exactTarget) {
        automaticPageTurnActive = false;
        // A current-layout page can still differ from the stored page
        // fingerprint after a firmware/cache rebuild (the layout identity
        // is unchanged, but serialized geometry may have been regenerated).
        // Reuse the bounded exact matcher instead of rejecting a valid
        // highlight immediately. This also lets a migrated v3 clipping
        // acquire a v4 layout fingerprint, but only after exactly one text
        // match is found inside the bounded window.
        constexpr uint16_t SEARCH_RADIUS = 4;
        jump.searchFirstPage = jump.page > SEARCH_RADIUS ? static_cast<uint16_t>(jump.page - SEARCH_RADIUS) : 0;
        jump.searchLastPage = static_cast<uint16_t>(std::min<uint32_t>(
            static_cast<uint32_t>(section->pageCount - 1), static_cast<uint32_t>(jump.page) + SEARCH_RADIUS));
        pendingClippingReanchorLaunch.store(true, std::memory_order_release);
        LOG_DBG("ERS", "Clipping page fingerprint changed; re-anchoring around page %u", jump.page);
        signalReadingPageHidden();
        requestUpdate();
        return;
      }
      pendingClippingJump.reset();
    }

    // The classifier only inspects serialized page metadata and source hrefs.
    // It must run before renderContents(), where a cache miss would extract and
    // decode the full EPUB image synchronously.
    if (skipCoverPageIfNeeded(*p)) return;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t renderStartedMs = static_cast<uint32_t>(millis());
#endif
    clippingHighlightsTruncated = renderContents(std::move(p), orientedMarginTop, orientedMarginRight,
                                                 orientedMarginBottom, orientedMarginLeft, &renderedPageFingerprint);
    signalReadingPageVisible();
    if (pendingSafeModePersistence.exchange(false, std::memory_order_acq_rel)) {
      if (persistBookReaderSettings()) {
        pendingSafeModeEnabledNotice = true;
      } else {
        pendingBookSettingsSaveError = true;
      }
    }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    LOG_DBG("ERS", "Rendered page in %ums", static_cast<unsigned>(static_cast<uint32_t>(millis()) - renderStartedMs));
    debugReportVisibleSection();
#endif
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot.exchange(false, std::memory_order_acq_rel)) {
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (!section || (!cachedContentSourceOffset.has_value() && cachedChapterTotalPageCount == 0)) return false;

  if (currentSpineIndex != cachedSpineIndex) {
    cachedContentSourceOffset.reset();
    cachedChapterTotalPageCount = 0;
    section->clearSourceOffsetTarget();
    return false;
  }

  const std::optional<uint16_t> contentPage =
      cachedContentSourceOffset.has_value() ? section->sourceOffsetTargetPage() : std::nullopt;
  if (section->isBuilding() && !contentPage.has_value()) return false;

  int newPage = section->currentPage;
  bool mapped = false;
  if (contentPage.has_value()) {
    newPage = *contentPage;
    mapped = true;
  } else if (cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
    // Image-only pages do not have a stable text anchor. Keep the old proportional fallback
    // rather than inventing a content position that cannot be identified safely.
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    mapped = true;
  }

  if (newPage < 0) newPage = 0;
  if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
    newPage = section->pageCount - 1;
  }

  const bool changed = mapped && newPage != section->currentPage;
  if (changed) section->currentPage = newPage;

  cachedContentSourceOffset.reset();
  cachedChapterTotalPageCount = 0;
  section->clearSourceOffsetTarget();
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
#define EPUB_RENDER_TIMESTAMP(name) const uint32_t name = static_cast<uint32_t>(millis())
#else
#define EPUB_RENDER_TIMESTAMP(name)
#endif

bool EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, uint32_t* const pageFingerprintOut) {
  EPUB_RENDER_TIMESTAMP(t0);
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  EPUB_RENDER_TIMESTAMP(tPrewarm);

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // The first page after a silent restart normally receives a HALF refresh.
  // Preserve that clean base for image pages, whose double-FAST path bypasses
  // the ordinary refresh cadence.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool darkReaderPage = SETTINGS.readerDarkMode != 0;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && !darkReaderPage;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool supportsGrayscale = renderer.supportsStripGrayscale();
  const uint32_t pageFingerprint =
      clippingStore.isLoaded() && clippingStore.size() > 0
          ? ClippingPageTools::fingerprint(*page, renderer, fontId, orientedMarginLeft, orientedMarginTop)
          : 0;
  if (pageFingerprintOut) *pageFingerprintOut = pageFingerprint;
  const ClippingPageTools::HighlightPlan clippingHighlights =
      pageFingerprint != 0 && section
          ? ClippingPageTools::buildHighlightPlan(renderer, *page, fontId, orientedMarginLeft, orientedMarginTop,
                                                  clippingStore.entries(), static_cast<uint16_t>(currentSpineIndex),
                                                  static_cast<uint16_t>(section->currentPage), pageFingerprint,
                                                  currentClippingLayoutFingerprint())
          : ClippingPageTools::HighlightPlan{};
  const bool underlineOnlyHighlights = SETTINGS.focusReadingEnabled != 0;
  const auto drawClippingBackgrounds = [&]() {
    if (!underlineOnlyHighlights) clippingHighlights.drawBackground(renderer);
  };
  const auto drawClippingUnderlines = [&]() { clippingHighlights.drawUnderline(renderer, !underlineOnlyHighlights); };
  auto renderGrayscalePass = [&]() {
    drawClippingBackgrounds();
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    drawClippingUnderlines();
  };

  if (pageHasImagesNeedingDecode) {
    drawClippingBackgrounds();
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    drawClippingUnderlines();
    renderStatusBar();
    if (darkReaderPage) renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  drawClippingBackgrounds();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  drawClippingUnderlines();
  renderStatusBar();
  EPUB_RENDER_TIMESTAMP(tBwRender);

  // Reader-only dark mode deliberately inverts the completed EPUB page at
  // the framebuffer boundary. This preserves pagination, glyph metrics,
  // image decoding and cache formats while leaving every non-reader screen
  // untouched. Grayscale text is disabled above because its two planes would
  // otherwise overwrite this inverse B/W frame.
  if (darkReaderPage) {
    renderer.invertScreen();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    EPUB_RENDER_TIMESTAMP(tDisplay);
    EPUB_RENDER_TIMESTAMP(tEnd);
    LOG_DBG("ERS", "Page render (dark): prewarm=%ums bw_render=%ums display=%ums total=%ums",
            static_cast<unsigned>(tPrewarm - t0), static_cast<unsigned>(tBwRender - tPrewarm),
            static_cast<unsigned>(tDisplay - tBwRender), static_cast<unsigned>(tEnd - t0));
    return clippingHighlights.truncated;
  }

  if (pageHasImages && supportsGrayscale) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      if (cleanImageBasePending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      drawClippingBackgrounds();
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      drawClippingUnderlines();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  EPUB_RENDER_TIMESTAMP(tDisplay);

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && supportsGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      EPUB_RENDER_TIMESTAMP(tGrayLsb);

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      EPUB_RENDER_TIMESTAMP(tGrayMsb);

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      EPUB_RENDER_TIMESTAMP(tGrayDisplay);

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      EPUB_RENDER_TIMESTAMP(tCleanup);

      EPUB_RENDER_TIMESTAMP(tEnd);
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%ums bw_render=%ums display=%ums gray_lsb=%ums "
              "gray_msb=%ums gray_display=%ums cleanup=%ums total=%ums",
              static_cast<unsigned>(tPrewarm - t0), static_cast<unsigned>(tBwRender - tPrewarm),
              static_cast<unsigned>(tDisplay - tBwRender), static_cast<unsigned>(tGrayLsb - tDisplay),
              static_cast<unsigned>(tGrayMsb - tGrayLsb), static_cast<unsigned>(tGrayDisplay - tGrayMsb),
              static_cast<unsigned>(tCleanup - tGrayDisplay), static_cast<unsigned>(tEnd - t0));
    }
  } else {
    // The B/W frame was already displayed above. Controllers without strip
    // grayscale support must not receive dual-plane operations.
    EPUB_RENDER_TIMESTAMP(tEnd);
    LOG_DBG("ERS", "Page render: prewarm=%ums bw_render=%ums display=%ums total=%ums",
            static_cast<unsigned>(tPrewarm - t0), static_cast<unsigned>(tBwRender - tPrewarm),
            static_cast<unsigned>(tDisplay - tBwRender), static_cast<unsigned>(tEnd - t0));
  }
  return clippingHighlights.truncated;
}

#undef EPUB_RENDER_TIMESTAMP

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    char formattedInterval[24];
    snprintf(formattedInterval, sizeof(formattedInterval), tr(STR_AUTO_TURN_SECONDS_FORMAT),
             static_cast<unsigned>(autoPageTurnSeconds));
    title = tr(STR_AUTO_TURN_ENABLED) + std::string(formattedInterval);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  bookmarksWritable = false;
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string canonicalPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(epub->getPath());
  const std::string bmPath = BookmarkUtil::canonicalFamilyExists(epub->getPath()) ? canonicalPath : legacyPath;
  BookmarkBookMetadata metadata;
  const JsonSettingsIO::BookmarkLoadStatus loaded =
      JsonSettingsIO::loadBookmarksFromFile(cachedBookmarks, bmPath.c_str(), &metadata);
  if (loaded == JsonSettingsIO::BookmarkLoadStatus::Loaded || loaded == JsonSettingsIO::BookmarkLoadStatus::Missing) {
    bookmarksWritable =
        BookmarkUtil::metadataMatchesBook(metadata, epub->getPath(), BookmarkEntry::PositionKind::Epub) &&
        std::all_of(cachedBookmarks.begin(), cachedBookmarks.end(), [](const BookmarkEntry& bookmark) {
          return bookmark.positionKind == BookmarkEntry::PositionKind::Epub;
        });
    if (!bookmarksWritable) {
      cachedBookmarks.clear();
      LOG_ERR("ERS", "Bookmark metadata does not belong to this EPUB");
    } else if (loaded == JsonSettingsIO::BookmarkLoadStatus::Loaded && metadata.path.empty()) {
      metadata = {epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub"};
      if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, canonicalPath.c_str(), &metadata)) {
        LOG_ERR("ERS", "Could not add book metadata to legacy bookmarks");
      }
    }
  } else {
    cachedBookmarks.clear();
    LOG_ERR("ERS", "Bookmark state is not writable after load failure (%u)", static_cast<unsigned>(loaded));
  }
  updateBookmarkFlag();
}

bool EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return false;
  }
  if (!bookmarksWritable) {
    pendingBookmarkStorageError = true;
    requestUpdate();
    return false;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);
  std::optional<uint32_t> sourceOffset = currentPageSourceOffset;
  if (!sourceOffset.has_value() && currentPage >= 0 && currentPage < pageCount) {
    if (const auto page = section->loadPage(currentPage)) sourceOffset = PageSourceAnchor::first(*page);
  }

  const std::vector<BookmarkEntry> previousBookmarks = cachedBookmarks;
  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange, sourceOffset);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    if (sourceOffset.has_value()) {
      entry.hasContentSourceOffset = true;
      entry.contentSourceOffset = *sourceOffset;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const BookmarkBookMetadata metadata{epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub"};
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str(), &metadata);
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
    cachedBookmarks = previousBookmarks;
    updateBookmarkFlag();
    pendingBookmarkStorageError = true;
  }
  requestUpdate();
  return ok;
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange,
                                   currentPageSourceOffset);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
