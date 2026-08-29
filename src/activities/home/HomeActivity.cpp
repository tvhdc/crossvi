#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/SourceIdentityStore.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/home/HomeShortcutsActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressFileCodec.h"
#include "activities/reader/ReadingStatsCompletionTransaction.h"
#include "activities/reader/ReadingStatsMenuActivity.h"
#include "components/UITheme.h"
#include "components/themes/crossvi/CrossViLayout.h"
#include "components/themes/crossvi/CrossViTheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t HOME_COVER_WORK_IDLE_MS = 2000;
constexpr size_t SOURCE_FINGERPRINT_BYTES_PER_STEP = 16U * 1024U;
constexpr size_t XTC_RECORDS_PER_STEP = 4;

struct DashboardProgressValidationContext {
  const std::string* cachePath = nullptr;
  int spineCount = 0;
  mutable int cachedSpineIndex = -1;
  mutable std::optional<uint16_t> cachedPageCount;
};

bool validateDashboardProgressCandidate(const uint8_t* data, const size_t size, const void* rawContext) {
  if (!rawContext) return false;
  const auto& context = *static_cast<const DashboardProgressValidationContext*>(rawContext);
  if (!context.cachePath) return false;

  DashboardProgress::Position position;
  if (!DashboardProgress::decode(data, size, position)) return false;
  if (context.cachedSpineIndex != static_cast<int>(position.spineIndex)) {
    context.cachedSpineIndex = static_cast<int>(position.spineIndex);
    context.cachedPageCount = Section::getCachedPageCount(*context.cachePath, static_cast<int>(position.spineIndex));
  }
  return DashboardProgress::validate(position, context.spineCount, context.cachedPageCount);
}

StrId homeBookHintLabelId(const HomeBookSummary& summary) {
  switch (homeBookAction(summary)) {
    case HomeBookAction::Continue:
      return StrId::STR_CONTINUE;
    case HomeBookAction::ReadAgain:
      return StrId::STR_READ_AGAIN;
    case HomeBookAction::Start:
      return StrId::STR_START;
    case HomeBookAction::Open:
    default:
      return StrId::STR_OPEN;
  }
}

}  // namespace

HomeActivity::HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const HomeMenuItem initialMenuItemValue)
    : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}

HomeActivity::~HomeActivity() = default;

int HomeActivity::getMenuItemCount() const {
  return HomeMenuMapping::selectionCount(static_cast<int>(recentBooks.size()), hasOpdsServers,
                                         hasReadingStatsShortcut());
}

bool HomeActivity::usesTripleCoverLayout() const {
  return SETTINGS.homeLayout == CrossPointSettings::HOME_LAYOUT_STYLE_3;
}

bool HomeActivity::usesCarouselLayout() const { return SETTINGS.homeLayout == CrossPointSettings::HOME_LAYOUT_STYLE_4; }

bool HomeActivity::usesMultiBookCoverLayout() const { return usesTripleCoverLayout() || usesCarouselLayout(); }

bool HomeActivity::usesRecentListLayout() const {
  return SETTINGS.homeLayout == CrossPointSettings::HOME_LAYOUT_STYLE_1;
}

void HomeActivity::selectHomeItem(const int index) {
  const bool selectionChanged = selectorIndex != index;
  selectorIndex = index;
  if (selectionChanged) {
    coverPreparationAttempted = false;
    preparedEpub.reset();
    preparedXtc.reset();
    preparedTxt.reset();
  }
  if (usesMultiBookCoverLayout() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()) &&
      carouselBookIndex != selectorIndex) {
    carouselBookIndex = selectorIndex;
    coverPreparationAttempted = false;
    coverRendered = false;
    freeCoverBuffer();
  }
  requestUpdate();
}

bool HomeActivity::hasReadingStatsShortcut() const { return true; }

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Keep the launcher consistent with My Books when the user opted out of
    // plain-text entries.  RecentBooksActivity already applies the same
    // filter to both of its tabs.
    if (SETTINGS.hideTxtBooks && FsHelpers::hasTxtExtension(book.path)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadBookSummary() {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t summaryStartedMs = static_cast<uint32_t>(millis());
  uint32_t summaryStageStartedMs = summaryStartedMs;
  const auto logSummaryTiming = [&](const char* stage) {
    const uint32_t now = static_cast<uint32_t>(millis());
    LOG_DBG("HOMT", "summary stage=%s elapsed_ms=%u total_ms=%u", stage,
            static_cast<unsigned>(now - summaryStageStartedMs), static_cast<unsigned>(now - summaryStartedMs));
    summaryStageStartedMs = now;
  };
#else
  const auto logSummaryTiming = [](const char*) {};
#endif

  bookSummary = {};
  if (recentBooks.empty()) {
    logSummaryTiming("no_book");
    return;
  }

  if (!FsHelpers::hasEpubExtension(recentBooks.front().path)) {
    logSummaryTiming("non_epub");
    return;
  }

  // Epub's constructor only derives the cache key. Dashboard verifies the
  // durable source binding once and then opens only the existing book.bin;
  // it never indexes book contents or performs the Reader's second
  // central-directory check during Home startup.
  if (!preparedEpub || preparedEpub->getPath() != recentBooks[0].path) {
    preparedEpub.reset(new (std::nothrow) Epub(recentBooks[0].path, "/.crosspoint"));
  }
  if (!preparedEpub) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    logSummaryTiming("epub_allocation_failed");
    return;
  }
  Epub& recentEpub = *preparedEpub;

  // Validate the metadata against the backing EPUB before reading any
  // path-keyed statistics or progress. A replaced, legacy, unreadable, or
  // otherwise invalid cache stays unknown on Home; Reader performs any safe
  // rebuild/reset when the user opens the book.
  const Epub::SourceBindingStatus sourceBinding = recentEpub.inspectSourceBinding();
  logSummaryTiming("source_binding");
  if (sourceBinding != Epub::SourceBindingStatus::Match) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    logSummaryTiming("source_binding_invalid");
    return;
  }

  const BookMetadataCache::LoadStatus metadataStatus = recentEpub.inspectCache();
  logSummaryTiming("metadata_cache");
  if (metadataStatus != BookMetadataCache::LoadStatus::Loaded) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    logSummaryTiming("metadata_cache_invalid");
    return;
  }
  BookReadingStats::LoadStatus statsStatus = BookReadingStats::LoadStatus::Missing;
  const BookReadingStats stats = BookReadingStats::load(recentEpub.getCachePath(), &statsStatus);
  logSummaryTiming("book_stats");
  bookSummary.bookStatsState = !BookReadingStats::isTrustedLoadStatus(statsStatus) ? DashboardMetricState::Unavailable
                               : statsStatus == BookReadingStats::LoadStatus::Missing ? DashboardMetricState::NoData
                                                                                      : DashboardMetricState::Available;
  if (bookSummary.bookStatsState == DashboardMetricState::Available) {
    bookSummary.bookReadingSeconds = stats.totalReadingSeconds;
    bookSummary.hasStartedReading =
        stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.sessionCount > 0 || stats.isCompleted;
  }

  // Completion is authoritative user state after Epub::load() has verified
  // that this cache still belongs to the EPUB at the recent path. It must not
  // disappear merely because a rebuild cleared the derived section cache or a
  // progress file is unavailable.
  if (DashboardProgress::fromCompletedStats(BookReadingStats::isTrustedLoadStatus(statsStatus), stats.isCompleted,
                                            bookSummary.progressPercent)) {
    bookSummary.hasProgress = true;
    bookSummary.hasStartedReading = true;
    bookSummary.progressState = DashboardMetricState::Available;
    logSummaryTiming("completed");
    return;
  }

  std::array<uint8_t, ProgressFile::EPUB_CONTENT_ANCHORED_PROGRESS_SIZE> progressBytes{};
  const DashboardProgressValidationContext progressContext{&recentEpub.getCachePath(), recentEpub.getSpineItemsCount()};
  const ProgressFile::CandidateValidator progressValidator{validateDashboardProgressCandidate, &progressContext};
  const ProgressFile::LoadResult progressLoad =
      ProgressFile::loadEpub(recentEpub.getCachePath(), progressBytes.data(), progressBytes.size(), progressValidator);
  logSummaryTiming("progress_file");
  // Legacy four-byte progress has no persisted chapter total, so it cannot
  // support an honest Dashboard percentage. Both the six-byte CrossVi layout
  // and CrossPoint's compatible ten-byte layout persist that total.
  if (!progressLoad || (progressLoad.size != ProgressFile::EPUB_PROGRESS_SIZE &&
                        progressLoad.size != ProgressFile::EPUB_CONTENT_ANCHORED_PROGRESS_SIZE)) {
    bookSummary.progressState = progressLoad.source == ProgressFile::LoadSource::Missing
                                    ? DashboardMetricState::NoData
                                    : DashboardMetricState::Unavailable;
    logSummaryTiming("progress_unavailable");
    return;
  }

  DashboardProgress::Position progress;
  if (!DashboardProgress::decode(progressBytes.data(), progressLoad.size, progress)) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    logSummaryTiming("progress_decode_failed");
    return;
  }

  const float chapterProgress = static_cast<float>(progress.pageNumber + 1U) / static_cast<float>(progress.pageCount);
  float bookProgress = 0.0F;
  if (!recentEpub.calculateProgressChecked(progress.spineIndex, chapterProgress, bookProgress) ||
      !DashboardProgress::toPercent(bookProgress, bookSummary.progressPercent)) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    logSummaryTiming("progress_calculation_failed");
    return;
  }
  bookSummary.hasProgress = true;
  bookSummary.progressBelowOnePercent = bookProgress > 0.0F && bookSummary.progressPercent == 0;
  bookSummary.hasStartedReading = bookSummary.hasStartedReading || progress.spineIndex > 0 || progress.pageNumber > 0;
  bookSummary.progressState = DashboardMetricState::Available;
  logSummaryTiming("done");
}

void HomeActivity::onEnter() {
  Activity::onEnter();
  coverPreparationAttempted = false;
  preparedEpub.reset();
  preparedXtc.reset();
  preparedTxt.reset();
  coverPreparationLastInputAt = static_cast<uint32_t>(millis());
  recentPruneIndex = 0;
  pinnedPruneIndex = 0;
  completionStatsAlreadyRecovered = false;

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t enterStartedMs = static_cast<uint32_t>(millis());
#endif

  const auto& metrics = UITheme::getInstance().getMetrics();
  mediaAvailable = Storage.probeMedia();
  if (mediaAvailable) {
    const ReadingStatsCompletionTransaction::RecoveryResult completionRecovery =
        ReadingStatsCompletionTransaction::recoverPending();
    completionStatsAlreadyRecovered = completionRecovery != ReadingStatsCompletionTransaction::RecoveryResult::Blocked;
    if (!completionStatsAlreadyRecovered) {
      LOG_ERR("HOME", "Pending reading-statistics transaction remains blocked");
    }
    recentBookLimit = metrics.homeRecentBooksCount;
    if (usesRecentListLayout()) {
      const int recentListTileHeight = CrossViMetrics::HOME_RECENT_LIST_TILE_HEIGHT;
      recentBookLimit = CrossViRecentListLayout::capacity(
          Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), recentListTileHeight});
    } else if (usesMultiBookCoverLayout()) {
      recentBookLimit = 3;
    }
    loadRecentBooks(recentBookLimit);
  } else {
    recentBooks.clear();
    recentBookLimit = 0;
  }
  recentPrunePending = mediaAvailable;
  hasOpdsServers = OPDS_STORE.hasServers();
  bookSummary = {};
  bookSummaryPending = !usesRecentListLayout() && mediaAvailable && !recentBooks.empty();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("HOMT", "onEnter before_request_update elapsed_ms=%u",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - enterStartedMs));
#endif

  selectorIndex = initialMenuItem == HomeMenuItem::NONE
                      ? 0
                      : HomeMenuMapping::selectorIndexOf(initialMenuItem, static_cast<int>(recentBooks.size()),
                                                         hasOpdsServers, hasReadingStatsShortcut());
  if (selectorIndex < 0) {
    // Preserve the previous fallback for an optional destination that vanished
    // while returning Home (for example, the last OPDS server was removed).
    selectorIndex = HomeMenuMapping::selectorIndexOf(HomeMenuItem::FILE_BROWSER, static_cast<int>(recentBooks.size()),
                                                     hasOpdsServers, hasReadingStatsShortcut());
  }
  carouselBookIndex = selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()) ? selectorIndex : 0;

  // Trigger first update
  requestUpdate();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("HOMT", "onEnter ready elapsed_ms=%u",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - enterStartedMs));
#endif
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  preparedEpub.reset();
  preparedXtc.reset();
  preparedTxt.reset();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::processRecentBooksMaintenance() {
  RenderLock lock(std::try_to_lock);
  if (!lock.ownsLock()) return;

  std::string removedPath;
  const RecentBooksStore::PruneStepResult result =
      RECENT_BOOKS.pruneMissingStep(recentPruneIndex, pinnedPruneIndex, &removedPath);
  if (result == RecentBooksStore::PruneStepResult::Pending) return;
  if (result == RecentBooksStore::PruneStepResult::Complete ||
      result == RecentBooksStore::PruneStepResult::MediaUnavailable ||
      result == RecentBooksStore::PruneStepResult::SaveFailed) {
    recentPrunePending = false;
    return;
  }

  const int oldRecentCount = static_cast<int>(recentBooks.size());
  const bool bookSelected = selectorIndex >= 0 && selectorIndex < oldRecentCount;
  const bool menuSelected = selectorIndex >= oldRecentCount;
  const HomeMenuItem selectedMenu =
      menuSelected ? indexToMenuItem(selectorIndex - oldRecentCount, hasOpdsServers, hasReadingStatsShortcut())
                   : HomeMenuItem::NONE;
  const std::string selectedBookPath = bookSelected ? recentBooks[selectorIndex].path : std::string{};

  loadRecentBooks(recentBookLimit);
  if (menuSelected) {
    selectorIndex = HomeMenuMapping::selectorIndexOf(selectedMenu, static_cast<int>(recentBooks.size()), hasOpdsServers,
                                                     hasReadingStatsShortcut());
  } else if (bookSelected) {
    const auto selected = std::find_if(recentBooks.begin(), recentBooks.end(),
                                       [&](const RecentBook& book) { return book.path == selectedBookPath; });
    selectorIndex = selected == recentBooks.end() ? std::min(selectorIndex, static_cast<int>(recentBooks.size()))
                                                  : static_cast<int>(std::distance(recentBooks.begin(), selected));
  } else {
    selectorIndex = 0;
  }
  carouselBookIndex = std::clamp(carouselBookIndex, 0, std::max(static_cast<int>(recentBooks.size()) - 1, 0));
  coverPreparationAttempted = false;
  coverRendered = false;
  freeCoverBuffer();
  preparedEpub.reset();
  preparedXtc.reset();
  preparedTxt.reset();

  bookSummary = {};
  bookSummaryPending = !usesRecentListLayout() && !recentBooks.empty();
  requestUpdate();
}

void HomeActivity::loop() {
  const bool hadInput = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased();
  const bool homeInputHeld = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                             mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                             mappedInput.isPressed(MappedInputManager::Button::NavNext) ||
                             mappedInput.isPressed(MappedInputManager::Button::NavPrevious);
  if (hadInput) {
    coverPreparationLastInputAt = static_cast<uint32_t>(millis());
  }
  const bool coverWorkIdle =
      static_cast<uint32_t>(static_cast<uint32_t>(millis()) - coverPreparationLastInputAt) >= HOME_COVER_WORK_IDLE_MS;

  // Source validation and summary I/O are not needed to paint the launcher.
  // Run them only after the first frame and the same quiet period used by
  // derived cover work. RenderLock protects bookSummary from render().
  bool sourcePreparationInProgress = false;
  if (bookSummaryPending && firstRenderDone && !hadInput && !homeInputHeld && coverWorkIdle) {
    const SourcePreparationResult preparation = stepRecentNonEpubSummarySource();
    sourcePreparationInProgress = preparation == SourcePreparationResult::InProgress;
    bool summaryReady = false;
    if (!sourcePreparationInProgress) {
      RenderLock lock(std::try_to_lock);
      if (lock.ownsLock() && firstRenderDone) {
        bookSummaryPending = false;
        if (preparation == SourcePreparationResult::Ready) {
          if (!loadRecentNonEpubReadingStats()) loadBookSummary();
        } else {
          loadBookSummary();
        }
        summaryReady = true;
      }
    }
    if (summaryReady) {
      requestUpdate();
      return;
    }
  }

  const bool focusedRecentBook = selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size());
  const int coverBookIndex = usesMultiBookCoverLayout() ? carouselBookIndex : 0;
  const bool x3 = renderer.getDisplayHeight() == 528;
  const bool needsShared = CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel = CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  if (!needsShared && !needsCarousel && !bookSummaryPending && firstRenderDone && !hadInput && !homeInputHeld &&
      coverWorkIdle && !sourcePreparationInProgress && focusedRecentBook && !coverPreparationAttempted &&
      mediaAvailable && FsHelpers::hasEpubExtension(recentBooks[static_cast<size_t>(selectorIndex)].path)) {
    coverPreparationAttempted = true;
    const std::string& path = recentBooks[static_cast<size_t>(selectorIndex)].path;
    const SourcePreparationResult preparation = stepPreparedEpub(path);
    if (preparation == SourcePreparationResult::InProgress) coverPreparationAttempted = false;
  }
  const int carouselWidth = x3 ? Epub::CAROUSEL_THUMB_WIDTH : Epub::CAROUSEL_X4_THUMB_WIDTH;
  const int carouselHeight = x3 ? Epub::CAROUSEL_THUMB_HEIGHT : Epub::CAROUSEL_X4_THUMB_HEIGHT;
  if ((needsShared || needsCarousel) && firstRenderDone && !hadInput && !homeInputHeld && coverWorkIdle &&
      !sourcePreparationInProgress && focusedRecentBook && !coverPreparationAttempted && mediaAvailable &&
      coverBookIndex >= 0 && coverBookIndex < static_cast<int>(recentBooks.size()) &&
      (FsHelpers::hasEpubExtension(recentBooks[coverBookIndex].path) ||
       FsHelpers::hasXtcExtension(recentBooks[coverBookIndex].path))) {
    coverPreparationAttempted = true;
    // A focused book is redrawn independently from the menu snapshot. Release
    // the stale snapshot before decoding so it cannot reduce available heap.
    if (usesMultiBookCoverLayout()) freeCoverBuffer();
    bool coverReady = false;
    if (FsHelpers::hasEpubExtension(recentBooks[coverBookIndex].path)) {
      const SourcePreparationResult preparation = stepPreparedEpub(recentBooks[coverBookIndex].path);
      if (preparation == SourcePreparationResult::InProgress) {
        coverPreparationAttempted = false;
      } else if (preparation == SourcePreparationResult::Ready) {
        const Epub::ThumbnailSetStatus thumbnails = preparedEpub->ensureThumbnails(
            Epub::ThumbnailRequest{needsShared, needsCarousel, x3}, Epub::ThumbnailMode::EmbeddedOnly);
        coverReady = needsCarousel ? thumbnails.carousel == Epub::ThumbnailStatus::Ready
                                   : thumbnails.shared == Epub::ThumbnailStatus::Ready;
      }
    } else {
      const std::string& path = recentBooks[coverBookIndex].path;
      const SourcePreparationResult preparation = stepPreparedXtc(path);
      if (preparation == SourcePreparationResult::InProgress) {
        coverPreparationAttempted = false;
      } else if (preparation == SourcePreparationResult::Ready) {
        const Xtc::ThumbnailPreparationStatus status =
            preparedXtc->thumbnailPreparationActive()
                ? preparedXtc->stepThumbnailPreparation(1024, 8)
                : preparedXtc->beginThumbnailPreparation(carouselWidth, carouselHeight);
        if (status == Xtc::ThumbnailPreparationStatus::InProgress) {
          coverPreparationAttempted = false;
        } else {
          coverReady = status == Xtc::ThumbnailPreparationStatus::Ready;
        }
      }
    }
    if (coverReady) {
      coverRendered = false;
      freeCoverBuffer();
      requestUpdate();
    }
  }

  const int menuCount = getMenuItemCount();
  const auto nextItem = [this, menuCount] { selectHomeItem(ButtonNavigator::nextIndex(selectorIndex, menuCount)); };
  const auto previousItem = [this, menuCount] {
    selectHomeItem(ButtonNavigator::previousIndex(selectorIndex, menuCount));
  };
  buttonNavigator.onNext(nextItem);
  buttonNavigator.onPrevious(previousItem);

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // backPressSeen guards against the stale release of the Back press that
  // closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen) {
    backPressSeen = false;
    const HomeMenuItem returnMenuItem = selectorIndex < static_cast<int>(recentBooks.size())
                                            ? HomeMenuItem::NONE
                                            : indexToMenuItem(selectorIndex - static_cast<int>(recentBooks.size()),
                                                              hasOpdsServers, hasReadingStatsShortcut());
    startActivityForResult(std::make_unique<HomeShortcutsActivity>(renderer, mappedInput, returnMenuItem),
                           [](const ActivityResult&) {});
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex, hasOpdsServers, hasReadingStatsShortcut())) {
        case HomeMenuItem::FILE_BROWSER:
          onFileBrowserOpen();
          break;
        case HomeMenuItem::RECENTS:
          onYourBooksOpen();
          break;
        case HomeMenuItem::SAVED_ITEMS:
          onSavedItemsOpen();
          break;
        case HomeMenuItem::OPDS_BROWSER:
          onOpdsBrowserOpen();
          break;
        case HomeMenuItem::FILE_TRANSFER:
          onFileTransferOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        case HomeMenuItem::READING_STATS:
          onReadingStatsOpen();
          break;
        default:
          break;
      }
    }
  }

  // External SD edits are uncommon, so verify one stored path per idle loop
  // after the first frame instead of probing every recent/pinned entry on the
  // launcher and reader-open critical paths.
  if (!hadInput && !homeInputHeld && recentPrunePending && firstRenderDone && coverWorkIdle) {
    processRecentBooksMaintenance();
  }
}

void HomeActivity::render(RenderLock&&) {
  if (renderBookLoadingOverlay()) return;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (!firstRenderDone) LOG_DBG("HOMT", "first_render_start");
#endif
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool carouselLayout = usesCarouselLayout();
  const bool tripleCoverLayout = usesTripleCoverLayout();
  const bool recentListLayout = usesRecentListLayout();
  const bool standardLayout = !carouselLayout && !tripleCoverLayout && !recentListLayout;
  const bool focusedRecentBook = selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size());
  const int homeTileHeight =
      recentListLayout ? CrossViMetrics::HOME_RECENT_LIST_TILE_HEIGHT : metrics.homeCoverTileHeight;
  const Rect homeTile{0, metrics.homeTopPadding, pageWidth, homeTileHeight};
  const Rect carouselContent{0, metrics.homeTopPadding, pageWidth,
                             std::max(0, pageHeight - metrics.homeTopPadding - metrics.buttonHintsHeight)};

  if (tripleCoverLayout) {
    coverRectX = homeTile.x;
    coverRectY = homeTile.y;
    coverRectW = homeTile.width;
    coverRectH = homeTile.height;
  } else if (carouselLayout) {
    const CrossViCarouselLayout layout = CrossViCarouselLayout::calculate(carouselContent);
    coverRectX = carouselContent.x;
    coverRectY = carouselContent.y;
    coverRectW = carouselContent.width;
    coverRectH = std::max(0, layout.menu.y - carouselContent.y);
  }

  renderer.clearScreen();
  const bool restoreMultiBookArea = (tripleCoverLayout || carouselLayout) && !focusedRecentBook;
  bool bufferRestored = (standardLayout || restoreMultiBookArea) && coverBufferStored && restoreCoverBuffer();

  GUI.drawHomeHeader(
      renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
      metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  constexpr size_t MAX_HOME_MENU_ITEMS = static_cast<size_t>(HomeMenuMapping::itemCount(true, true) + 1);
  std::array<const char*, MAX_HOME_MENU_ITEMS> menuItems{};
  std::array<UIIcon, MAX_HOME_MENU_ITEMS> menuIcons{};
  size_t menuCount = 0;
  const auto addMenuItem = [&](const char* label, const UIIcon icon) {
    menuItems[menuCount] = label;
    menuIcons[menuCount++] = icon;
  };
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) addMenuItem(tr(STR_CONTINUE_READING), Book);
  addMenuItem(tr(STR_BROWSE_FILES), Folder);
  addMenuItem(tr(STR_MENU_RECENT_BOOKS), Recent);
  if (hasReadingStatsShortcut()) addMenuItem(tr(STR_READING_STATS), Book);
  addMenuItem(tr(STR_SAVED_ITEMS), Bookmark);
  if (hasOpdsServers) addMenuItem(tr(STR_OPDS_BROWSER), Library);
  addMenuItem(tr(STR_FILE_TRANSFER), Transfer);
  addMenuItem(tr(STR_SETTINGS_TITLE), Settings);

  if (carouselLayout) {
    static_cast<const CrossViTheme&>(GUI).drawHomeCarousel(
        renderer, carouselContent, recentBooks, carouselBookIndex,
        selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()), static_cast<int>(menuCount),
        selectorIndex - static_cast<int>(recentBooks.size()), [&menuIcons](int index) { return menuIcons[index]; },
        !bufferRestored);
    if (restoreMultiBookArea && !bufferRestored) coverBufferStored = storeCoverBuffer();
  } else {
    // Record the tile rect so storeCoverBuffer (called from the theme) knows
    // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
    // instead of the 48 KB full framebuffer the previous bind captured.
    if (recentListLayout) {
      coverRectX = 0;
      coverRectY = 0;
      coverRectW = 0;
      coverRectH = 0;
      static_cast<const CrossViTheme&>(GUI).drawHomeRecentList(
          renderer, homeTile, recentBooks,
          selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()) ? selectorIndex : -1);
    } else if (tripleCoverLayout) {
      if (!bufferRestored) {
        static_cast<const CrossViTheme&>(GUI).drawHomeTripleCovers(renderer, homeTile, recentBooks, carouselBookIndex,
                                                                   focusedRecentBook);
        if (restoreMultiBookArea) coverBufferStored = storeCoverBuffer();
      }
    } else {
      const Rect cacheRect = GUI.getHomeCoverCacheRect(homeTile);
      coverRectX = cacheRect.x;
      coverRectY = cacheRect.y;
      coverRectW = cacheRect.width;
      coverRectH = cacheRect.height;

      GUI.drawHomeContent(renderer, homeTile, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this), bookSummary);
    }

    const int menuTop = metrics.homeTopPadding + homeTileHeight + metrics.homeMenuTopOffset;
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    GUI.drawButtonMenu(
        renderer, Rect{0, menuTop, pageWidth, std::max(0, contentBottom - menuTop)}, static_cast<int>(menuCount),
        metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
        [&menuItems](int index) { return menuItems[index]; }, [&menuIcons](int index) { return menuIcons[index]; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_SHORTCUTS), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
  firstRenderDone = true;
}

void HomeActivity::onSelectBook(const std::string& path) {
  RawSourceIdentityHandoff preparedIdentity;
  if (preparedEpub && preparedEpub->getPath() == path && preparedEpub->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedEpub), ReaderOpenOrigin::HomeRecent, completionStatsAlreadyRecovered);
    return;
  } else if (preparedXtc && preparedXtc->getPath() == path && preparedXtc->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedXtc), ReaderOpenOrigin::HomeRecent, completionStatsAlreadyRecovered);
    return;
  } else if (preparedTxt && preparedTxt->getPath() == path && preparedTxt->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedTxt), ReaderOpenOrigin::HomeRecent, completionStatsAlreadyRecovered);
    return;
  }
  openBookWithFeedback(path, ReaderOpenOrigin::HomeRecent, completionStatsAlreadyRecovered);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onYourBooksOpen() { activityManager.goToYourBooks(); }

void HomeActivity::onSavedItemsOpen() { activityManager.goToSavedClippings(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

HomeActivity::SourcePreparationResult HomeActivity::stepPreparedEpub(const std::string& path) {
  preparedXtc.reset();
  preparedTxt.reset();
  if (!preparedEpub || preparedEpub->getPath() != path) {
    preparedEpub.reset(new (std::nothrow) Epub(path, "/.crosspoint"));
    if (!preparedEpub || !preparedEpub->beginCoreMetadataRead()) {
      preparedEpub.reset();
      return SourcePreparationResult::Failed;
    }
  }

  if (preparedEpub->hasPreparedCoreMetadata()) return SourcePreparationResult::Ready;
  if (!preparedEpub->isReadingCoreMetadata() && !preparedEpub->beginCoreMetadataRead()) {
    preparedEpub.reset();
    return SourcePreparationResult::Failed;
  }

  BookMetadataCache::BookMetadata metadata;
  const Epub::CoreMetadataStepResult result = preparedEpub->stepCoreMetadataRead(metadata);
  if (result == Epub::CoreMetadataStepResult::InProgress) return SourcePreparationResult::InProgress;
  if (result == Epub::CoreMetadataStepResult::Loaded) return SourcePreparationResult::Ready;
  preparedEpub.reset();
  return SourcePreparationResult::Failed;
}

HomeActivity::SourcePreparationResult HomeActivity::stepPreparedXtc(const std::string& path) {
  preparedEpub.reset();
  preparedTxt.reset();
  if (!preparedXtc || preparedXtc->getPath() != path) {
    preparedXtc.reset(new (std::nothrow) Xtc(path, "/.crosspoint"));
    if (!preparedXtc || !preparedXtc->beginLoad()) {
      preparedXtc.reset();
      return SourcePreparationResult::Failed;
    }
  }
  if (preparedXtc->isLoaded()) return SourcePreparationResult::Ready;

  const Xtc::LoadStepResult result = preparedXtc->stepLoad(XTC_RECORDS_PER_STEP, SOURCE_FINGERPRINT_BYTES_PER_STEP);
  if (result == Xtc::LoadStepResult::InProgress) return SourcePreparationResult::InProgress;
  if (result == Xtc::LoadStepResult::Loaded) return SourcePreparationResult::Ready;
  preparedXtc.reset();
  return SourcePreparationResult::Failed;
}

HomeActivity::SourcePreparationResult HomeActivity::stepPreparedTxt(const std::string& path) {
  preparedEpub.reset();
  preparedXtc.reset();
  if (!preparedTxt || preparedTxt->getPath() != path) {
    preparedTxt.reset(new (std::nothrow) Txt(path, "/.crosspoint"));
    if (!preparedTxt || !preparedTxt->beginLoad()) {
      preparedTxt.reset();
      return SourcePreparationResult::Failed;
    }
  }
  if (preparedTxt->isLoaded()) return SourcePreparationResult::Ready;

  const Txt::LoadStepResult result = preparedTxt->stepLoad(SOURCE_FINGERPRINT_BYTES_PER_STEP);
  if (result == Txt::LoadStepResult::InProgress) return SourcePreparationResult::InProgress;
  if (result == Txt::LoadStepResult::Loaded) return SourcePreparationResult::Ready;
  preparedTxt.reset();
  return SourcePreparationResult::Failed;
}

HomeActivity::SourcePreparationResult HomeActivity::stepRecentNonEpubSummarySource() {
  if (recentBooks.empty()) return SourcePreparationResult::NotNeeded;
  const std::string& path = recentBooks.front().path;
  if (FsHelpers::hasXtcExtension(path)) return stepPreparedXtc(path);
  if (!FsHelpers::hasTxtExtension(path) && !FsHelpers::hasMarkdownExtension(path)) {
    return SourcePreparationResult::NotNeeded;
  }

  return stepPreparedTxt(path);
}

bool HomeActivity::loadRecentNonEpubReadingStats() {
  if (recentBooks.empty()) return false;
  const std::string& path = recentBooks.front().path;
  std::string cachePath;
  ZipFile::SourceIdentity currentIdentity;
  ZipFile::SourceIdentity storedIdentity;
  const bool isXtc = FsHelpers::hasXtcExtension(path);
  uint32_t xtcPageCount = 0;
  size_t txtFileSize = 0;

  if (isXtc) {
    if (!preparedXtc || preparedXtc->getPath() != path || !preparedXtc->isLoaded() ||
        !preparedXtc->getSourceIdentity(currentIdentity)) {
      return false;
    }
    cachePath = preparedXtc->getCachePath();
    xtcPageCount = preparedXtc->getPageCount();
    if (xtcPageCount == 0) return false;
  } else {
    if (!preparedTxt || preparedTxt->getPath() != path || !preparedTxt->isLoaded() ||
        !preparedTxt->getSourceIdentity(currentIdentity)) {
      return false;
    }
    cachePath = preparedTxt->getCachePath();
    txtFileSize = preparedTxt->getFileSize();
  }

  const SourceIdentityStore::LoadStatus identityStatus = SourceIdentityStore::load(cachePath, storedIdentity);
  const bool identityTrusted = identityStatus == SourceIdentityStore::LoadStatus::Primary ||
                               identityStatus == SourceIdentityStore::LoadStatus::Backup ||
                               identityStatus == SourceIdentityStore::LoadStatus::Temp;
  if (!identityTrusted || storedIdentity != currentIdentity) return false;

  if (isXtc) {
    uint8_t progressBytes[4]{};
    const ProgressFile::PageBounds bounds{xtcPageCount};
    const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
    const ProgressFile::LoadResult loaded =
        ProgressFile::loadPage(cachePath, progressBytes, sizeof(progressBytes), validator);
    if (loaded) {
      const uint32_t page = ProgressFileCodec::decodePage(progressBytes);
      const uint32_t percent =
          static_cast<uint32_t>(std::min<uint64_t>(100, (static_cast<uint64_t>(page) + 1u) * 100u / xtcPageCount));
      bookSummary.hasProgress = true;
      bookSummary.progressBelowOnePercent = percent == 0;
      bookSummary.hasStartedReading = page > 0;
      bookSummary.progressPercent = static_cast<uint8_t>(percent);
      bookSummary.progressState = DashboardMetricState::Available;
    } else {
      bookSummary.progressState = loaded.source == ProgressFile::LoadSource::Missing
                                      ? DashboardMetricState::NoData
                                      : DashboardMetricState::Unavailable;
    }
  } else if (txtFileSize > 0) {
    uint8_t progressBytes[ProgressFileCodec::TXT_V2_SIZE]{};
    const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(txtFileSize), 0};
    const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
    const ProgressFile::LoadResult loaded =
        ProgressFile::loadTxt(cachePath, progressBytes, sizeof(progressBytes), validator);
    uint32_t byteOffset = 0;
    if (loaded && ProgressFileCodec::decodeTxt(progressBytes, loaded.size, byteOffset) ==
                      ProgressFileCodec::TxtDecodeStatus::Ok) {
      const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(byteOffset) * 100u / txtFileSize);
      bookSummary.hasProgress = true;
      bookSummary.hasStartedReading = byteOffset > 0;
      bookSummary.progressEstimated = true;
      bookSummary.progressPercent = static_cast<uint8_t>(std::min<uint32_t>(percent, 100u));
      bookSummary.progressState = DashboardMetricState::Available;
    } else {
      bookSummary.progressState = loaded.source == ProgressFile::LoadSource::Missing
                                      ? DashboardMetricState::NoData
                                      : DashboardMetricState::Unavailable;
    }
  } else {
    bookSummary.progressState = DashboardMetricState::NoData;
  }

  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Missing;
  const BookReadingStats bookStats = BookReadingStats::load(cachePath, &bookStatus);
  if (!BookReadingStats::isTrustedLoadStatus(bookStatus)) return false;

  bookSummary.bookStatsState = bookStatus == BookReadingStats::LoadStatus::Missing ? DashboardMetricState::NoData
                                                                                   : DashboardMetricState::Available;
  bookSummary.bookReadingSeconds = bookStats.totalReadingSeconds;
  bookSummary.hasStartedReading = bookSummary.hasStartedReading || bookStats.totalReadingSeconds > 0 ||
                                  bookStats.totalPagesTurned > 0 || bookStats.sessionCount > 0 || bookStats.isCompleted;

  if (bookStats.isCompleted) {
    bookSummary.hasProgress = true;
    bookSummary.hasStartedReading = true;
    bookSummary.progressEstimated = false;
    bookSummary.progressPercent = 100;
    bookSummary.progressState = DashboardMetricState::Available;
  }

  return true;
}

void HomeActivity::onReadingStatsOpen() {
  startActivityForResult(std::make_unique<ReadingStatsMenuActivity>(renderer, mappedInput),
                         [](const ActivityResult&) {});
}
