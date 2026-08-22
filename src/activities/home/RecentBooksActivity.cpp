#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>

#include "CrossPointSettings.h"
#include "FinishedBooksStore.h"
#include "MappedInputManager.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookSavedItemsActivity.h"
#include "activities/reader/BookStatsLoader.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsActivity.h"
#include "activities/reader/ReadingStatsCompletionTransaction.h"
#include "activities/reader/ReadingStatsPresentation.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/LibraryGridModel.h"
#include "components/LibraryGridView.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"

namespace {
constexpr unsigned long LONG_PRESS_MS = 500;
constexpr uint32_t NAVIGATION_RELEASE_GUARD_MS = 150;
constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr unsigned long CATALOG_LOOP_BUDGET_MS = 8;

enum class BookAction : uint8_t { Open, Stats, Saved, ClearCache, Completion, Pin, RemoveRecent, Delete };

std::string bookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return Epub(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasXtcExtension(path)) return Xtc(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return {};
}

bool loadCompletionState(const LibraryBookRecord& record, std::string& cachePath, BookReadingStats& bookStats,
                         GlobalReadingStats& globalStats) {
  ReadingStatsPresentation ignored;
  if (!loadBookStatsPresentation({record.path, record.title, record.author, record.coverBmpPath}, ignored))
    return false;
  cachePath = bookCachePath(record.path);
  if (cachePath.empty()) return false;
  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  bookStats = BookReadingStats::load(cachePath, &bookStatus);
  globalStats = GlobalReadingStats::load(&globalStatus);
  return BookReadingStats::isTrustedLoadStatus(bookStatus) && GlobalReadingStats::isTrustedLoadStatus(globalStatus);
}

bool setBookCompletion(const LibraryBookRecord& record, const bool completed) {
  std::string cachePath;
  BookReadingStats oldBook;
  GlobalReadingStats oldGlobal;
  if (!loadCompletionState(record, cachePath, oldBook, oldGlobal) || oldBook.isCompleted == completed) return false;
  BookReadingStats nextBook = oldBook;
  GlobalReadingStats nextGlobal = oldGlobal;
  nextBook.isCompleted = completed;
  if (completed) {
    nextBook.estimatedTimeLeftSeconds = 0;
    if (!nextBook.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        nextBook.finishedDate = now.date;
        nextBook.finishedMinuteOfDay = static_cast<uint16_t>(now.hour) * 60u + now.minute;
      }
    }
    nextGlobal.completedBooks = addReadingStatsSaturated(nextGlobal.completedBooks, 1);
  } else {
    nextBook.finishedDateManual = false;
    nextBook.finishedDate.clear();
    nextBook.finishedMinuteOfDay = BookReadingStats::INVALID_MINUTE_OF_DAY;
    if (nextGlobal.completedBooks > 0) --nextGlobal.completedBooks;
  }
  if (!ReadingStatsCompletionTransaction::commit(cachePath, oldBook, nextBook, oldGlobal, nextGlobal)) return false;
  if (completed) {
    FINISHED_BOOKS.markCompleted(record.path, record.title, record.author,
                                 nextBook.finishedDate.isValid() ? readingStatsDayIndex(nextBook.finishedDate) : 0);
  } else {
    FINISHED_BOOKS.removeByPath(record.path);
  }
  return true;
}

BookSavedItemsActivity::ReaderKind savedItemsKind(const std::string& path) {
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return BookSavedItemsActivity::ReaderKind::Text;
  }
  return FsHelpers::hasXtcExtension(path) ? BookSavedItemsActivity::ReaderKind::FixedLayout
                                          : BookSavedItemsActivity::ReaderKind::Epub;
}
constexpr uint8_t CATALOG_STEPS_PER_LOOP = 8;
constexpr size_t SEARCH_RECORDS_PER_STEP = 8;
constexpr int LIBRARY_BOTTOM_GAP = 8;
constexpr unsigned long CACHED_COVER_BATCH_BUDGET_MS = 75;
constexpr uint32_t COVER_WORK_IDLE_MS = 2000;
static_assert(LibraryGridModel::pageSize(0) <= 8, "Cover-ready mask must fit the library page");

#if defined(ENABLE_SERIAL_LOG)
const char* libraryTabName(const bool all) { return all ? "all" : "recent"; }

const char* catalogPhaseName(const LibraryCatalogStore::Phase phase) {
  switch (phase) {
    case LibraryCatalogStore::Phase::Idle:
      return "idle";
    case LibraryCatalogStore::Phase::Discovering:
      return "discover";
    case LibraryCatalogStore::Phase::Enriching:
      return "metadata";
    case LibraryCatalogStore::Phase::Sorting:
      return "publish";
    case LibraryCatalogStore::Phase::Ready:
      return "ready";
    case LibraryCatalogStore::Phase::Error:
      return "error";
    case LibraryCatalogStore::Phase::Updating:
      return "update";
  }
  return "unknown";
}

const char* orderPhaseName(const LibraryCatalogStore::OrderPhase phase) {
  switch (phase) {
    case LibraryCatalogStore::OrderPhase::Idle:
      return "idle";
    case LibraryCatalogStore::OrderPhase::Initializing:
      return "seed";
    case LibraryCatalogStore::OrderPhase::Merging:
      return "merge";
    case LibraryCatalogStore::OrderPhase::Publishing:
      return "publish";
  }
  return "unknown";
}
#endif

std::string fallbackBookTitle(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string title = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t extension = title.find_last_of('.');
  if (extension != std::string::npos) title.resize(extension);
  std::replace(title.begin(), title.end(), '_', ' ');
  return title;
}

LibraryBookFormat formatForPath(const std::string& path) {
  if (FsHelpers::hasTxtExtension(path)) return LibraryBookFormat::Text;
  if (FsHelpers::hasMarkdownExtension(path)) return LibraryBookFormat::Markdown;
  if (FsHelpers::checkFileExtension(path, ".xtch")) return LibraryBookFormat::Xtch;
  if (FsHelpers::checkFileExtension(path, ".xtc")) return LibraryBookFormat::Xtc;
  return LibraryBookFormat::Epub;
}

bool hasCachedNoCoverMarker(const LibraryBookRecord& book) {
  if (book.format != LibraryBookFormat::Epub || book.coverBmpPath.empty()) return false;
  const std::string sharedPath = UITheme::getCoverThumbPath(book.coverBmpPath, Epub::SHARED_THUMB_HEIGHT);
  return Storage.exists((sharedPath + ".nocover").c_str());
}

const char* formatLabel(const LibraryBookFormat format) {
  switch (format) {
    case LibraryBookFormat::Text:
      return "TXT";
    case LibraryBookFormat::Markdown:
      return "Markdown";
    case LibraryBookFormat::Xtc:
      return "XTC";
    case LibraryBookFormat::Xtch:
      return "XTCH";
    case LibraryBookFormat::Epub:
    default:
      return "EPUB";
  }
}
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::optional<YourBooksReturnState> returnState)
    : Activity("YourBooks", renderer, mappedInput), pendingReturnState(std::move(returnState)) {}

RecentBooksActivity::~RecentBooksActivity() = default;

#if defined(ENABLE_SERIAL_LOG)
void RecentBooksActivity::traceCatalogState(const char* const event) {
  const uint32_t now = static_cast<uint32_t>(millis());
  const LibraryCatalogStore::Phase phase = LIBRARY_CATALOG.phase();
  if (phase != libraryTraceCatalogPhase) {
    LOG_DBG("LIBT", "catalog_phase event=%s from=%s to=%s phase_ms=%u tab_ms=%u count=%u", event,
            catalogPhaseName(libraryTraceCatalogPhase), catalogPhaseName(phase),
            static_cast<unsigned>(now - libraryTracePhaseStartedAt),
            static_cast<unsigned>(now - libraryTraceTabStartedAt), static_cast<unsigned>(LIBRARY_CATALOG.count()));
    libraryTraceCatalogPhase = phase;
    libraryTracePhaseStartedAt = now;
  }

  const LibraryCatalogStore::OrderPhase order = LIBRARY_CATALOG.orderPhase();
  if (order == libraryTraceOrderPhase) return;
  if (libraryTraceOrderPhase == LibraryCatalogStore::OrderPhase::Idle) {
    libraryTraceOrderStartedAt = now;
    libraryTraceOrderPhaseStartedAt = now;
  }
  LOG_DBG("LIBT", "order_phase event=%s from=%s to=%s phase_ms=%u total_ms=%u count=%u sort=%u", event,
          orderPhaseName(libraryTraceOrderPhase), orderPhaseName(order),
          static_cast<unsigned>(now - libraryTraceOrderPhaseStartedAt),
          static_cast<unsigned>(now - libraryTraceOrderStartedAt), static_cast<unsigned>(LIBRARY_CATALOG.count()),
          static_cast<unsigned>(SETTINGS.librarySort));
  libraryTraceOrderPhase = order;
  libraryTraceOrderPhaseStartedAt = now;
}

void RecentBooksActivity::traceNavigationQueued(const char* const kind, const int delta) {
  const uint32_t now = static_cast<uint32_t>(millis());
  const uint32_t id = ++libraryTraceNextNavigationId;
  if (libraryTraceQueuedFirstId == 0) {
    libraryTraceQueuedFirstId = id;
    libraryTraceQueuedAt = now;
  }
  libraryTraceQueuedLastId = id;
  const size_t count = visibleBookCount();
  const size_t capacity = pageCapacity();
  const size_t selected =
      LibraryGridModel::clampIndex(bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], count);
  const size_t page = count == 0 ? 0 : LibraryGridModel::pageStart(selected, count, capacity);
  LOG_DBG("LIBT", "nav_input id=%u kind=%s delta=%d tab=%s focus=%u page=%u pending=%d/%d/%d render_pending=%u",
          static_cast<unsigned>(id), kind, delta, libraryTabName(allTab()), static_cast<unsigned>(selectorIndex),
          static_cast<unsigned>(page), pendingNavigation, pendingTabSwitch, pendingPageSwitch,
          activityManager.hasPendingRender() ? 1U : 0U);
}
#endif

void RecentBooksActivity::loadRecentBooks() {
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t startedAt = static_cast<uint32_t>(millis());
#endif
  recentBooks.clear();
  const auto& stored = RECENT_BOOKS.getBooks();
  recentBooks.reserve(stored.size());
  for (const auto& path : RECENT_BOOKS.getPinnedPaths()) {
    if (SETTINGS.hideTxtBooks && FsHelpers::hasTxtExtension(path)) continue;
    const auto metadata =
        std::find_if(stored.begin(), stored.end(), [&path](const RecentBook& book) { return book.path == path; });
    if (metadata != stored.end()) recentBooks.push_back(*metadata);
  }
  for (const auto& book : stored) {
    if (SETTINGS.hideTxtBooks && FsHelpers::hasTxtExtension(book.path)) continue;
    if (!RECENT_BOOKS.isPinned(book.path)) {
      recentBooks.push_back(book);
    }
  }
  invalidateRenderPage();
  resetCoverQueue();
#if defined(ENABLE_SERIAL_LOG)
  LOG_DBG("LIBT", "recent_projection count=%u stored=%u pinned=%u hide_txt=%u elapsed_ms=%u",
          static_cast<unsigned>(recentBooks.size()), static_cast<unsigned>(stored.size()),
          static_cast<unsigned>(RECENT_BOOKS.getPinnedPaths().size()), static_cast<unsigned>(SETTINGS.hideTxtBooks),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedAt));
#endif
}

void RecentBooksActivity::rebuildPinnedProjection() {
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t startedAt = static_cast<uint32_t>(millis());
#endif
  pinnedSourceIndices.clear();
  allSourceIndices.clear();
  allSourceIndicesValid = false;
  allSourceIndicesHideTxt = SETTINGS.hideTxtBooks;
  allSourceIndicesSort = SETTINGS.librarySort;
  pinnedProjectionGeneration = LIBRARY_CATALOG.generation();
  pinnedProjectionValid = true;
  if (!allTab() || !storageAvailable || searchActive[tabIndex()] || !LIBRARY_CATALOG.isReady()) return;

  const auto excluded = SETTINGS.hideTxtBooks ? LibraryBookFormat::Text : static_cast<LibraryBookFormat>(0xFF);
  const auto& paths = RECENT_BOOKS.getPinnedPaths();
  std::vector<size_t> resolved;
  if (!LIBRARY_CATALOG.loadOrderedIndices(SETTINGS.librarySort, excluded, allSourceIndices, paths, &resolved)) {
    pinnedProjectionValid = false;
#if defined(ENABLE_SERIAL_LOG)
    LOG_DBG("LIBT", "projection ready=0 order_building=%u catalog=%u visible=0 pinned=%u elapsed_ms=%u",
            static_cast<unsigned>(LIBRARY_CATALOG.isOrderBuilding()), static_cast<unsigned>(LIBRARY_CATALOG.count()),
            static_cast<unsigned>(paths.size()), static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedAt));
    traceCatalogState("projection");
#endif
    return;
  }
  allSourceIndicesValid = true;

  if (!paths.empty()) {
    for (const size_t index : resolved) {
      if (index == static_cast<size_t>(-1)) continue;
      if (SETTINGS.hideTxtBooks &&
          std::find(allSourceIndices.begin(), allSourceIndices.end(), index) == allSourceIndices.end()) {
        continue;
      }
      pinnedSourceIndices.push_back(index);
    }
  }
#if defined(ENABLE_SERIAL_LOG)
  LOG_DBG("LIBT", "projection ready=1 order_building=%u catalog=%u visible=%u pinned=%u elapsed_ms=%u",
          static_cast<unsigned>(LIBRARY_CATALOG.isOrderBuilding()), static_cast<unsigned>(LIBRARY_CATALOG.count()),
          static_cast<unsigned>(allSourceIndices.size()), static_cast<unsigned>(pinnedSourceIndices.size()),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedAt));
  traceCatalogState("projection");
#endif
}

bool RecentBooksActivity::pinnedProjectionCurrent() const {
  return !allTab() || searchActive[tabIndex()] ||
         (pinnedProjectionValid && allSourceIndicesValid && allSourceIndicesHideTxt == SETTINGS.hideTxtBooks &&
          allSourceIndicesSort == SETTINGS.librarySort && pinnedProjectionGeneration == LIBRARY_CATALOG.generation());
}

size_t RecentBooksActivity::allVisibleToSource(const size_t visibleBookIndex) const {
  const size_t sourceCount = LIBRARY_CATALOG.count();
  const size_t visibleSourceCount = allSourceIndices.size();
  if (visibleBookIndex >= visibleSourceCount) return static_cast<size_t>(-1);
  if (pinnedSourceIndices.empty() || visibleBookIndex < pinnedSourceIndices.size()) {
    const size_t source =
        pinnedSourceIndices.empty() ? allSourceIndices[visibleBookIndex] : pinnedSourceIndices[visibleBookIndex];
    return source < sourceCount ? source : static_cast<size_t>(-1);
  }

  size_t nonPinnedOrdinal = visibleBookIndex - pinnedSourceIndices.size();
  // allSourceIndices is already in the user's chosen order. Do not walk raw
  // catalog ordinals here: doing so would silently undo title/author/date
  // sorting after the pinned prefix.
  for (const size_t source : allSourceIndices) {
    if (source >= sourceCount) continue;
    const bool pinned =
        std::find(pinnedSourceIndices.begin(), pinnedSourceIndices.end(), source) != pinnedSourceIndices.end();
    if (!pinned) {
      if (nonPinnedOrdinal == 0) return source;
      --nonPinnedOrdinal;
    }
  }
  return static_cast<size_t>(-1);
}

size_t RecentBooksActivity::allSourceToVisible(const size_t sourceBookIndex) const {
  if (sourceBookIndex >= LIBRARY_CATALOG.count()) return static_cast<size_t>(-1);
  const auto sourceIt = std::find(allSourceIndices.begin(), allSourceIndices.end(), sourceBookIndex);
  if (sourceIt == allSourceIndices.end()) {
    return static_cast<size_t>(-1);
  }
  for (size_t visible = 0; visible < pinnedSourceIndices.size(); ++visible) {
    if (pinnedSourceIndices[visible] == sourceBookIndex) return visible;
  }
  size_t before = 0;
  const size_t baseOrdinal = static_cast<size_t>(std::distance(allSourceIndices.begin(), sourceIt));
  for (size_t index = 0; index < baseOrdinal; ++index) {
    if (std::find(pinnedSourceIndices.begin(), pinnedSourceIndices.end(), allSourceIndices[index]) !=
        pinnedSourceIndices.end()) {
      ++before;
    }
  }
  return pinnedSourceIndices.size() + baseOrdinal - before;
}

size_t RecentBooksActivity::visibleBookCount() const {
  if (!storageAvailable) return 0;
  const size_t ti = tabIndex();
  if (allTab() && catalogOpenPending) return 0;
  if (allTab() && !searchActive[ti] && LIBRARY_CATALOG.isReady() && !pinnedProjectionCurrent()) return 0;
  if (allTab() && searchActive[ti] && !allSearchJob.running &&
      allSearchResultGeneration != LIBRARY_CATALOG.generation()) {
    return 0;
  }
  if (searchActive[ti]) return searchResults[ti].size();
  return allTab() ? (SETTINGS.hideTxtBooks ? allSourceIndices.size() : LIBRARY_CATALOG.count()) : recentBooks.size();
}

size_t RecentBooksActivity::sourceIndex(const size_t visibleBookIndex) const {
  const size_t ti = tabIndex();
  if (!searchActive[ti]) {
    if (allTab()) return pinnedProjectionCurrent() ? allVisibleToSource(visibleBookIndex) : static_cast<size_t>(-1);
    return visibleBookIndex;
  }
  if (allTab() && !allSearchJob.running && allSearchResultGeneration != LIBRARY_CATALOG.generation()) {
    return static_cast<size_t>(-1);
  }
  return visibleBookIndex < searchResults[ti].size() ? searchResults[ti][visibleBookIndex] : static_cast<size_t>(-1);
}

bool RecentBooksActivity::loadVisibleBook(const size_t visibleBookIndex, LibraryBookRecord& book) const {
  const size_t source = sourceIndex(visibleBookIndex);
  if (allTab()) {
    if (source == static_cast<size_t>(-1) || !LIBRARY_CATALOG.loadRecord(source, book)) return false;
    book.pinned = RECENT_BOOKS.isPinned(book.path);
    return true;
  }
  if (source >= recentBooks.size()) return false;
  const RecentBook& recent = recentBooks[source];
  book.path = recent.path;
  book.title = recent.title.empty() ? fallbackBookTitle(recent.path) : recent.title;
  book.author = recent.author;
  book.coverBmpPath = recent.coverBmpPath;
  book.format = formatForPath(recent.path);
  book.sourceSize = 0;
  book.pinned = RECENT_BOOKS.isPinned(book.path);
  return true;
}

void RecentBooksActivity::loadRenderPage(const size_t pageStart, const size_t count) {
  if (renderPageValid && renderPageTab == tab && renderPageStart == pageStart && renderPageCount == count) return;
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t startedAt = static_cast<uint32_t>(millis());
#endif

  // The all-books catalog is fixed-size records. Read one visible page with a
  // single file handle instead of opening/closing the catalog once per cover.
  // Search results are non-contiguous, so retain the existing indexed path for
  // that case. The page is then reused while only the focused book changes.
  bool loaded = true;
  if (allTab() && !searchActive[tabIndex()]) {
    if (!pinnedProjectionCurrent()) {
      loaded = false;
      renderPage.assign(count, {});
    } else {
      std::array<size_t, LibraryGridModel::LIST_PAGE_SIZE> sourceIndices{};
      loaded = count <= sourceIndices.size() && LibraryGridModel::collectVisiblePageSources(
                                                    allSourceIndices, pinnedSourceIndices, LIBRARY_CATALOG.count(),
                                                    pageStart, std::span<size_t>(sourceIndices).first(count));
      if (loaded) {
        loaded = LIBRARY_CATALOG.loadRecords(std::span<const size_t>(sourceIndices).first(count), renderPage);
      }
    }
  } else {
    renderPage.resize(count);
    for (size_t i = 0; i < count; ++i) {
      if (!loadVisibleBook(pageStart + i, renderPage[i])) renderPage[i] = {};
    }
  }
  for (auto& book : renderPage) book.pinned = RECENT_BOOKS.isPinned(book.path);
  if (!loaded) renderPage.assign(count, {});
  renderPageValid = true;
  renderPageStart = pageStart;
  renderPageCount = count;
  renderPageTab = tab;
#if defined(ENABLE_SERIAL_LOG)
  LOG_DBG("LIBT", "page_load tab=%s start=%u requested=%u loaded=%u ok=%u elapsed_ms=%u", libraryTabName(allTab()),
          static_cast<unsigned>(pageStart), static_cast<unsigned>(count), static_cast<unsigned>(renderPage.size()),
          static_cast<unsigned>(loaded), static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedAt));
#endif
}

uint8_t RecentBooksActivity::viewMode() const { return SETTINGS.libraryView; }

uint8_t RecentBooksActivity::gridMode() const { return SETTINGS.libraryGrid; }

size_t RecentBooksActivity::pageCapacity() const {
  if (viewMode() == CrossPointSettings::LIBRARY_COVERS) return LibraryGridView::pageSize(gridMode());
  return LibraryGridModel::LIST_PAGE_SIZE;
}

void RecentBooksActivity::rememberCurrentBook() {
  if (!bookSelected()) return;
  const size_t ti = tabIndex();
  rememberedBookIndex[ti] = selectedBookIndex();
  if (renderPageValid && renderPageTab == tab && rememberedBookIndex[ti] >= renderPageStart &&
      rememberedBookIndex[ti] < renderPageStart + renderPageCount) {
    rememberedBookPath[ti] = renderPage[rememberedBookIndex[ti] - renderPageStart].path;
    return;
  }
  LibraryBookRecord book;
  if (loadVisibleBook(rememberedBookIndex[ti], book)) rememberedBookPath[ti] = book.path;
}

void RecentBooksActivity::captureReaderReturnContext(const LibraryBookRecord& book) const {
  activityManager.captureYourBooksReturnContext(static_cast<uint8_t>(tab), selectedBookIndex(), book.path,
                                                searchActive[tabIndex()] ? searchQuery[tabIndex()] : std::string{});
}

void RecentBooksActivity::openSelectedBook(const std::string& path) {
  RawSourceIdentityHandoff preparedIdentity;
  if (preparedEpub && preparedEpub->getPath() == path && preparedEpub->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedEpub), ReaderOpenOrigin::Default);
    return;
  }
  if (preparedXtc && preparedXtc->getPath() == path && preparedXtc->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedXtc), ReaderOpenOrigin::Default);
    return;
  }
  const RawSourceIdentityHandoff* reusableIdentity = nullptr;
  if (preparedEpubSourceIdentity && preparedEpubSourceIdentity->path == path) {
    reusableIdentity = &*preparedEpubSourceIdentity;
  } else if (preparedXtcSourceIdentity && preparedXtcSourceIdentity->path == path) {
    reusableIdentity = &*preparedXtcSourceIdentity;
  }
  openBookWithFeedback(path, ReaderOpenOrigin::Default, false, reusableIdentity);
}

void RecentBooksActivity::restoreRememberedBook(const bool locateByPath) {
  const size_t count = visibleBookCount();
  if (count == 0) {
    selectorIndex = 0;
    if (!allTab() || !LIBRARY_CATALOG.isBuilding()) rememberedBookPath[tabIndex()].clear();
    return;
  }
  const size_t ti = tabIndex();
  size_t restored = LibraryGridModel::clampIndex(rememberedBookIndex[ti], count);
  if (locateByPath && !rememberedBookPath[ti].empty()) {
    const std::string path = rememberedBookPath[ti];
    if (allTab() && !searchActive[ti]) {
      size_t found = 0;
      const auto result = LIBRARY_CATALOG.findPath(path, restored, found);
      if (result == LibraryCatalogStore::FindPathResult::Found) {
        if (pinnedProjectionCurrent()) {
          const size_t visible = allSourceToVisible(found);
          if (visible != static_cast<size_t>(-1)) restored = visible;
        }
      } else if (result == LibraryCatalogStore::FindPathResult::IoError) {
        rememberedBookIndex[ti] = restored;
        selectorIndex = controlCount() + restored;
        popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
        popupTime = millis();
        requestUpdate();
        return;
      }
    } else {
      restored = LibraryGridModel::restoreIndex(restored, count, [this, &path](const size_t index) {
        LibraryBookRecord book;
        return loadVisibleBook(index, book) && book.path == path;
      });
    }
  }
  rememberedBookIndex[ti] = restored;
  selectorIndex = controlCount() + restored;
  LibraryBookRecord book;
  if (loadVisibleBook(restored, book)) rememberedBookPath[ti] = book.path;
}

void RecentBooksActivity::selectTab(const Tab next) {
  if (tab == next) return;
#if defined(ENABLE_SERIAL_LOG)
  const bool previousAll = allTab();
#endif
  if (allTab()) cancelAllSearch();
  rememberCurrentBook();
  tab = next;
#if defined(ENABLE_SERIAL_LOG)
  libraryTraceTabStartedAt = static_cast<uint32_t>(millis());
  libraryTraceFirstVisiblePending = true;
  LOG_DBG("LIBT", "tab_request from=%s to=%s catalog_phase=%s count=%u", libraryTabName(previousAll),
          libraryTabName(allTab()), catalogPhaseName(LIBRARY_CATALOG.phase()),
          static_cast<unsigned>(LIBRARY_CATALOG.count()));
#endif
  if (allTab()) pinnedProjectionValid = false;
  invalidateRenderPage();
  resetCoverQueue();
  // Do not probe/open the SD catalog in the Confirm edge path.  Opening a
  // large or freshly rebuilt catalog can take seconds on X3, so paint the new
  // tab immediately and finish catalog setup in the next loop iteration.
  catalogOpenPending = allTab();
  const bool warmAllBooks = catalogOpenPending && LIBRARY_CATALOG.isReady() && !LIBRARY_CATALOG.isOrderBuilding();
  preserveTabFocus = allTab();
  if (!allTab()) restoreRememberedBook();
  selectorIndex = 0;
  if (!warmAllBooks) requestUpdate();
}

bool RecentBooksActivity::refreshStorageAvailability() {
  storageAvailable = Storage.probeMedia();
  if (!storageAvailable) {
    resetCoverQueue();
    LIBRARY_CATALOG.invalidateSourceValidation();
    LIBRARY_CATALOG.cancel();
  }
  return storageAvailable;
}

void RecentBooksActivity::queueNavigationInput() {
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
    coverQueueLastInputAt = static_cast<uint32_t>(millis());
  }
  if (optionPopup.isActive()) {
    holdUp.reset();
    holdDown.reset();
    holdLeft.reset();
    holdRight.reset();
    holdBack.reset();
    buttonNavigator_.onPrevious([this] {
      --pendingPopupNavigation;
      requestUpdate();
    });
    buttonNavigator_.onNext([this] {
      ++pendingPopupNavigation;
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (suppressPopupConfirmRelease) {
        suppressPopupConfirmRelease = false;
        confirmPressSeen = false;
        confirmLongHandled = false;
      } else {
        pendingPopupConfirmRelease = true;
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      pendingPopupBackRelease = true;
      requestUpdate();
    }
    return;
  }

  if (popupMessage != StrId::STR_NONE_OPT || openingBook) {
    holdUp.reset();
    holdDown.reset();
    holdLeft.reset();
    holdRight.reset();
    holdBack.reset();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) holdUp.onPress();
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) holdDown.onPress();
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) holdLeft.onPress();
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) holdRight.onPress();
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) holdBack.onPress();

  // On X3, logical Up/Down are the physical left/right side buttons. Flip
  // only that device's tab direction; X4 retains its vertical convention.
  const int upTabDelta = gpio.deviceIsX3() ? -1 : 1;
  if (mappedInput.isPressed(MappedInputManager::Button::Up)) {
    (void)holdUp.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Up), LONG_PRESS_MS);
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Down)) {
    (void)holdDown.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Down), LONG_PRESS_MS);
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Left)) {
    (void)holdLeft.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Left), LONG_PRESS_MS);
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Right)) {
    (void)holdRight.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Right), LONG_PRESS_MS);
  }

  buttonNavigator_.onContinuous({MappedInputManager::Button::Up}, [this, upTabDelta] {
    pendingTabSwitch += upTabDelta;
#if defined(ENABLE_SERIAL_LOG)
    traceNavigationQueued("tab", upTabDelta);
#endif
    requestUpdate();
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Down}, [this, upTabDelta] {
    pendingTabSwitch -= upTabDelta;
#if defined(ENABLE_SERIAL_LOG)
    traceNavigationQueued("tab", -upTabDelta);
#endif
    requestUpdate();
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Right}, [this] {
    ++pendingPageSwitch;
#if defined(ENABLE_SERIAL_LOG)
    traceNavigationQueued("page", 1);
#endif
    requestUpdate();
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Left}, [this] {
    --pendingPageSwitch;
#if defined(ENABLE_SERIAL_LOG)
    traceNavigationQueued("page", -1);
#endif
    requestUpdate();
  });
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      holdBack.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Back), LONG_PRESS_MS)) {
    pendingSearch = true;
    requestUpdate();
  }

  const auto queueShortNavigation = [this](const int delta) {
    if (navigationReleaseGuard.accept(static_cast<uint32_t>(millis()), NAVIGATION_RELEASE_GUARD_MS)) {
      pendingNavigation += delta;
#if defined(ENABLE_SERIAL_LOG)
      traceNavigationQueued("cursor", delta);
#endif
    }
  };
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) &&
      holdUp.onRelease() == ReaderUtils::HoldRelease::Short) {
    queueShortNavigation(-1);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) &&
      holdDown.onRelease() == ReaderUtils::HoldRelease::Short) {
    queueShortNavigation(1);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) &&
      holdLeft.onRelease() == ReaderUtils::HoldRelease::Short) {
    queueShortNavigation(-1);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) &&
      holdRight.onRelease() == ReaderUtils::HoldRelease::Short) {
    queueShortNavigation(1);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (holdBack.onRelease() == ReaderUtils::HoldRelease::Short) pendingBack = true;
  }

  // A tab release can arrive while the render task owns the lock.  Clear the
  // swallow flag here so it cannot consume the first Confirm on a book after
  // the user has already moved focus away from the tab row.
  if (confirmTabHandled && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmTabHandled = false;
  }

  // Keep both edges of a book Confirm alive across a busy cover render.  The
  // hardware input manager exposes edges for one main-loop iteration only;
  // without this small latch a short press can disappear while RenderLock is
  // unavailable and the user has to press again.
  if (confirmBookPressCaptured && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    pendingBookConfirmRelease = true;
    requestUpdate();
    return;
  }

  // A tab Confirm is a state change, not a renderer-dependent action. Capture
  // its edge before trying the render lock so a slow cover read cannot consume
  // it. The actual tab mutation remains serialized below.
  if (!pendingTabConfirm && !confirmTabHandled && tabSelected() &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    pendingTabConfirm = true;
    confirmTabHandled = true;
    confirmPressSeen = true;
    requestUpdate();
    return;
  }

  if (!confirmBookPressCaptured && bookSelected() && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // A release edge may have been consumed while the previous tab render was
    // busy.  A new press while focus is on a book proves that the old click is
    // over, so it must not be swallowed as the tab release.
    confirmTabHandled = false;
    confirmBookPressCaptured = true;
    confirmPressSeen = true;
    confirmLongHandled = false;
    requestUpdate();
    return;
  }

  if (optionPopup.isActive() || popupMessage != StrId::STR_NONE_OPT || confirmPressSeen ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    return;
  }
}

void RecentBooksActivity::applyPendingNavigation() {
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t traceFirstId = libraryTraceQueuedFirstId;
  const uint32_t traceLastId = libraryTraceQueuedLastId;
  const uint32_t traceInputAt = libraryTraceQueuedAt;
#endif
  const Tab previousTab = tab;
  const size_t previousFocus = selectorIndex;
  const size_t previousCount = visibleBookCount();
  const size_t previousCapacity = pageCapacity();
  const size_t previousIndex = LibraryGridModel::clampIndex(
      bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], previousCount);
  const size_t previousPage = LibraryGridModel::pageStart(previousIndex, previousCount, previousCapacity);
  bool changed = false;

  while (pendingTabSwitch != 0) {
    const Tab next = pendingTabSwitch > 0 ? (tab == Tab::Recent ? Tab::All : Tab::Recent)
                                          : (tab == Tab::All ? Tab::Recent : Tab::All);
    selectTab(next);
    pendingTabSwitch += pendingTabSwitch > 0 ? -1 : 1;
    changed = true;
  }

  if (pendingPageSwitch != 0 && visibleBookCount() > 0) {
    const size_t count = visibleBookCount();
    const size_t capacity = pageCapacity();
    const size_t current =
        LibraryGridModel::clampIndex(bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], count);
    const size_t pageCount = (count + capacity - 1) / capacity;
    size_t page = current / capacity;
    const size_t slot = current % capacity;
    while (pendingPageSwitch > 0) {
      page = (page + 1) % pageCount;
      --pendingPageSwitch;
      changed = true;
    }
    while (pendingPageSwitch < 0) {
      page = page == 0 ? pageCount - 1 : page - 1;
      ++pendingPageSwitch;
      changed = true;
    }
    const size_t target = std::min(page * capacity + slot, count - 1);
    rememberedBookIndex[tabIndex()] = target;
    if (bookSelected()) selectorIndex = controlCount() + target;
    rememberCurrentBook();
  } else {
    pendingPageSwitch = 0;
  }

  if (pendingNavigation != 0) {
    const size_t count = visibleBookCount();
    const int maxFocus = count == 0 ? 0 : static_cast<int>(count);
    while (pendingNavigation < 0) {
      if (selectorIndex > 0) {
        selectorIndex = std::max<size_t>(0, selectorIndex - 1);
        changed = true;
      } else if (viewMode() == CrossPointSettings::LIBRARY_COVERS && count > 0) {
        const size_t anchor = LibraryGridModel::clampIndex(rememberedBookIndex[tabIndex()], count);
        const size_t target = LibraryGridModel::lastIndexOnPage(anchor, count, pageCapacity());
        selectorIndex = controlCount() + target;
        changed = true;
      }
      ++pendingNavigation;
    }
    while (pendingNavigation > 0) {
      if (selectorIndex == 0 && count > 0) {
        const size_t anchor = LibraryGridModel::clampIndex(rememberedBookIndex[tabIndex()], count);
        selectorIndex = controlCount() + LibraryGridModel::pageStart(anchor, count, pageCapacity());
        changed = true;
      } else if (selectorIndex < static_cast<size_t>(maxFocus)) {
        ++selectorIndex;
        changed = true;
      }
      --pendingNavigation;
    }
    if (bookSelected()) rememberCurrentBook();
  }

  if (changed) {
    preserveTabFocus = tabSelected();
    const size_t currentCount = visibleBookCount();
    const size_t currentCapacity = pageCapacity();
    const size_t currentIndex = LibraryGridModel::clampIndex(
        bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], currentCount);
    const size_t currentPage = LibraryGridModel::pageStart(currentIndex, currentCount, currentCapacity);
    // Moving inside an already rendered page only changes focus. Reusing the
    // page/cover snapshot avoids rereading or regenerating every cover for a
    // single button press; invalidate only when the visible page or tab truly
    // changes.
    const bool pageInvalidated = previousTab != tab || previousCount != currentCount || previousPage != currentPage;
    if (pageInvalidated) {
      invalidateRenderPage();
      resetCoverQueue();
    }
#if defined(ENABLE_SERIAL_LOG)
    if (traceFirstId != 0) {
      const uint32_t appliedAt = static_cast<uint32_t>(millis());
      LOG_DBG("LIBT", "nav_apply ids=%u-%u changed=1 wait_ms=%u tab=%s->%s focus=%u->%u page=%u->%u invalidate=%u",
              static_cast<unsigned>(traceFirstId), static_cast<unsigned>(traceLastId),
              static_cast<unsigned>(appliedAt - traceInputAt), libraryTabName(previousTab == Tab::All),
              libraryTabName(allTab()), static_cast<unsigned>(previousFocus), static_cast<unsigned>(selectorIndex),
              static_cast<unsigned>(previousPage), static_cast<unsigned>(currentPage), pageInvalidated ? 1U : 0U);
      if (libraryTraceVisibleFirstId == 0) {
        libraryTraceVisibleFirstId = traceFirstId;
        libraryTraceVisibleInputAt = traceInputAt;
      }
      libraryTraceVisibleLastId = traceLastId;
    }
#endif
    requestUpdate();
  }
#if defined(ENABLE_SERIAL_LOG)
  else if (traceFirstId != 0) {
    LOG_DBG("LIBT", "nav_apply ids=%u-%u changed=0 wait_ms=%u tab=%s focus=%u page=%u",
            static_cast<unsigned>(traceFirstId), static_cast<unsigned>(traceLastId),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - traceInputAt), libraryTabName(allTab()),
            static_cast<unsigned>(selectorIndex), static_cast<unsigned>(previousPage));
  }
  if (traceFirstId != 0) {
    libraryTraceQueuedFirstId = 0;
    libraryTraceQueuedLastId = 0;
    libraryTraceQueuedAt = 0;
  }
#endif
}

void RecentBooksActivity::invalidateRenderPage() {
  renderPageValid = false;
  renderPageStart = 0;
  renderPageCount = 0;
  invalidateGridSnapshot();
}

void RecentBooksActivity::invalidateGridSnapshot() { gridSnapshotValid = false; }

bool RecentBooksActivity::restoreGridSnapshot(const Rect rect, const size_t pageStart, const size_t pageCount,
                                              const uint8_t gridSetting) {
  if (!gridSnapshotValid || !gridSnapshot || gridSnapshotTab != tab || gridSnapshotStart != pageStart ||
      gridSnapshotCount != pageCount || gridSnapshotGrid != gridSetting || gridSnapshotRect.x != rect.x ||
      gridSnapshotRect.y != rect.y || gridSnapshotRect.width != rect.width || gridSnapshotRect.height != rect.height) {
    return false;
  }
  return renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, gridSnapshot, gridSnapshotSize);
}

bool RecentBooksActivity::storeGridSnapshot(const Rect rect, const size_t pageStart, const size_t pageCount,
                                            const uint8_t gridSetting) {
  const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
  if (needed == 0) return false;
  if (!gridSnapshot || gridSnapshotSize < needed) {
    free(gridSnapshot);
    gridSnapshot = static_cast<uint8_t*>(malloc(needed));
    gridSnapshotSize = gridSnapshot ? needed : 0;
  }
  if (!gridSnapshot ||
      !renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, gridSnapshot, gridSnapshotSize)) {
    gridSnapshotValid = false;
    return false;
  }
  gridSnapshotRect = rect;
  gridSnapshotStart = pageStart;
  gridSnapshotCount = pageCount;
  gridSnapshotTab = tab;
  gridSnapshotGrid = gridSetting;
  gridSnapshotValid = true;
  return true;
}

void RecentBooksActivity::freeGridSnapshot() {
  free(gridSnapshot);
  gridSnapshot = nullptr;
  gridSnapshotSize = 0;
  gridSnapshotValid = false;
}

void RecentBooksActivity::cancelCoverPreparation() {
  if (coverPreparationEpub) {
    coverPreparationEpub->cancelCoreMetadataRead();
    coverPreparationEpub->cancelThumbnailPreparation();
  }
  if (coverPreparationXtc) {
    coverPreparationXtc->cancelThumbnailPreparation();
    coverPreparationXtc->cancelLoad();
  }
  coverPreparationEpub.reset();
  coverPreparationXtc.reset();
  coverPreparationPath.clear();
}

void RecentBooksActivity::resetCoverQueue() {
  cancelCoverPreparation();
  coverQueuePageStart = static_cast<size_t>(-1);
  coverQueueCursor = 0;
  coverQueueReadyMask = 0;
  coverQueueShownMask = 0;
  coverQueueAbsentMask = 0;
}

bool RecentBooksActivity::coverCachesRequested() const {
  return CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView) ||
         CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
}

void RecentBooksActivity::processCoverQueue() {
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() || confirmBookPressCaptured ||
      pendingNavigation != 0 || pendingTabSwitch != 0 || pendingPageSwitch != 0 || pendingSearch || pendingBack ||
      pendingTabConfirm || pendingBookConfirmRelease) {
    return;
  }
  const bool drawsCovers = viewMode() == CrossPointSettings::LIBRARY_COVERS;
  const bool idle =
      LibraryGridModel::coverWorkIdle(static_cast<uint32_t>(millis()), coverQueueLastInputAt, COVER_WORK_IDLE_MS);
  const bool needsShared = CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel = CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  if (catalogLoading() || (!needsShared && !needsCarousel) || visibleBookCount() == 0 ||
      coverQueuePageStart == static_cast<size_t>(-1)) {
    return;
  }

  const size_t capacity = pageCapacity();
  const size_t selected = LibraryGridModel::clampIndex(
      bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], visibleBookCount());
  const size_t pageStart = LibraryGridModel::pageStart(selected, visibleBookCount(), capacity);
  if (pageStart != coverQueuePageStart || coverQueueCursor >= std::min(capacity, visibleBookCount() - pageStart)) {
    return;
  }

  const size_t pageCount = std::min(capacity, visibleBookCount() - pageStart);
  const unsigned long batchStarted = millis();
  while (coverQueueCursor < pageCount) {
    const size_t offset = LibraryGridModel::coverQueueOffset(coverQueueCursor, coverQueueSelected);
    if (offset >= pageCount) break;

    // loop() already owns RenderLock while this queue runs. Taking the same
    // non-recursive lock again would always fail and leave the cover cursor
    // permanently stuck at its first item.
    if (!renderPageValid || renderPageTab != tab || renderPageStart != pageStart || renderPageCount != pageCount ||
        offset >= renderPage.size()) {
      return;
    }
    const LibraryBookRecord book = renderPage[offset];
    if (book.path.empty()) {
#if defined(CROSSVI_COVER_DEBUG)
      LOG_INF("COVDBG", "cover record unavailable page_start=%u offset=%u catalog_building=%d",
              static_cast<unsigned>(pageStart), static_cast<unsigned>(offset), LIBRARY_CATALOG.isBuilding());
#endif
      if (drawsCovers) {
        const uint8_t readyBit = static_cast<uint8_t>(1U << offset);
        coverQueueReadyMask |= readyBit;
        coverQueueShownMask |= readyBit;
        coverQueueAbsentMask |= readyBit;
      }
      ++coverQueueCursor;
      break;
    }

    const uint8_t coverBit = static_cast<uint8_t>(1U << offset);
    const bool cachedNoCover = book.coverBmpPath.empty() || hasCachedNoCoverMarker(book);
    if (cachedNoCover) {
      if (drawsCovers) {
        coverQueueShownMask |= coverBit;
        coverQueueAbsentMask |= coverBit;
      }
      ++coverQueueCursor;
      continue;
    }

#if defined(CROSSVI_COVER_DEBUG)
    const uint32_t started = static_cast<uint32_t>(millis());
#endif
    // Text formats intentionally use their static placeholder, so they are
    // already ready and can share the same refresh batch as cached covers.
    bool generated =
        !drawsCovers || book.format == LibraryBookFormat::Text || book.format == LibraryBookFormat::Markdown;
    bool cachedBefore = true;
    if (!coverPreparationPath.empty() && coverPreparationPath != book.path) cancelCoverPreparation();
    if (book.format == LibraryBookFormat::Epub) {
      if (!coverPreparationEpub) {
        coverPreparationEpub.reset(new (std::nothrow) Epub(book.path, "/.crosspoint"));
        if (!coverPreparationEpub) {
          generated = false;
          if (drawsCovers) {
            const uint8_t readyBit = static_cast<uint8_t>(1U << offset);
            coverQueueReadyMask |= readyBit;
            coverQueueShownMask |= readyBit;
            coverQueueAbsentMask |= readyBit;
          }
          ++coverQueueCursor;
          break;
        }
        coverPreparationPath = book.path;
      }
      Epub& epub = *coverPreparationEpub;
      const std::string sharedPath = epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT);
      const int carouselHeight =
          renderer.getDisplayHeight() == 528 ? Epub::CAROUSEL_THUMB_HEIGHT : Epub::CAROUSEL_X4_THUMB_HEIGHT;
      const std::string carouselPath = epub.getThumbBmpPath(carouselHeight);
      const uint8_t readyBit = static_cast<uint8_t>(1U << offset);
      const bool sharedBitmapPresent = Storage.exists(sharedPath.c_str());
      const bool sharedNoCover = Storage.exists((sharedPath + ".nocover").c_str());
      const bool sharedRendered = (coverQueueShownMask & readyBit) != 0;
      const bool sharedCached =
          !needsShared || sharedNoCover || (sharedBitmapPresent && (!drawsCovers || sharedRendered));
      const bool carouselCached =
          !needsCarousel || Storage.exists(carouselPath.c_str()) || Storage.exists((carouselPath + ".nocover").c_str());
      cachedBefore = sharedCached && carouselCached;

      // The first page render already attempts every library-sized cache in a
      // single framebuffer update. Keep this fallback for an older snapshot,
      // but collect all hits into one refresh instead of refreshing e-ink once
      // per book.
      if (drawsCovers && sharedCached && (coverQueueShownMask & readyBit) == 0) {
        coverQueueShownMask |= readyBit;
        coverQueueReadyMask |= readyBit;
      }
      if (!cachedBefore && !idle) return;

      const auto capturePreparedIdentity = [&]() {
        if (offset != coverQueueSelected) return;
        RawSourceIdentityHandoff preparedIdentity;
        if (epub.getSourceIdentityHandoff(preparedIdentity)) {
          preparedEpubSourceIdentity = std::move(preparedIdentity);
        }
      };
      const auto finishEpubPreparation = [&]() {
        if (offset == coverQueueSelected) {
          RawSourceIdentityHandoff preparedIdentity;
          if (epub.getSourceIdentityHandoff(preparedIdentity)) {
            preparedEpubSourceIdentity = std::move(preparedIdentity);
            preparedEpub = std::move(coverPreparationEpub);
          }
        }
        coverPreparationEpub.reset();
        coverPreparationPath.clear();
      };
      const auto stepCoreMetadata = [&]() {
        if (epub.hasPreparedCoreMetadata()) return Epub::CoreMetadataStepResult::Loaded;
        if (!epub.isReadingCoreMetadata() && !epub.beginCoreMetadataRead()) {
          return Epub::CoreMetadataStepResult::Error;
        }
        BookMetadataCache::BookMetadata metadata;
        return epub.stepCoreMetadataRead(metadata);
      };

      const bool selectedIdentityNeeded =
          offset == coverQueueSelected &&
          (!preparedEpubSourceIdentity || preparedEpubSourceIdentity->path != book.path);
      if (selectedIdentityNeeded && epub.hasPreparedCoreMetadata()) capturePreparedIdentity();
      if (selectedIdentityNeeded && !epub.hasPreparedCoreMetadata()) {
        const Epub::CoreMetadataStepResult metadata = stepCoreMetadata();
        if (metadata == Epub::CoreMetadataStepResult::InProgress) return;
        if (metadata == Epub::CoreMetadataStepResult::Loaded) {
          capturePreparedIdentity();
          return;
        }
        generated = false;
        coverPreparationEpub.reset();
        coverPreparationPath.clear();
      } else if (cachedBefore) {
        capturePreparedIdentity();
        // Cache files that the production Bitmap reader has just displayed do
        // not need another EPUB central-directory scan merely to advance this
        // UI queue. Book replacement paths invalidate their derived cache.
        generated = true;
        finishEpubPreparation();
      } else if (coverPreparationEpub) {
        const Epub::ThumbnailRequest request{needsShared, needsCarousel, renderer.getDisplayHeight() == 528};
        const bool preparationWasActive = epub.thumbnailPreparationActive();
        Epub::ThumbnailPreparationStatus preparation =
            preparationWasActive ? epub.stepThumbnailPreparation() : epub.beginThumbnailPreparation(request);
        if (preparation == Epub::ThumbnailPreparationStatus::InProgress) return;
        if (preparation == Epub::ThumbnailPreparationStatus::NeedsCoreMetadata) {
          const Epub::CoreMetadataStepResult metadata = stepCoreMetadata();
          if (metadata == Epub::CoreMetadataStepResult::InProgress) return;
          if (metadata == Epub::CoreMetadataStepResult::Loaded) {
            capturePreparedIdentity();
            return;
          }
          preparation = Epub::ThumbnailPreparationStatus::Error;
        }

        if (preparation == Epub::ThumbnailPreparationStatus::Error) {
          generated = false;
          coverPreparationEpub.reset();
          coverPreparationPath.clear();
        } else if (preparation == Epub::ThumbnailPreparationStatus::Ready && !preparationWasActive) {
          // beginThumbnailPreparation() already validated both requested caches.
          generated = true;
          finishEpubPreparation();
        } else {
          const Epub::ThumbnailSetStatus thumbnails =
              epub.ensureThumbnails(request, Epub::ThumbnailMode::EmbeddedThenCover);
          const auto ready = [](const Epub::ThumbnailStatus status) {
            return status == Epub::ThumbnailStatus::Ready || status == Epub::ThumbnailStatus::NoCover;
          };
          generated = (!needsShared || ready(thumbnails.shared)) && (!needsCarousel || ready(thumbnails.carousel));
          capturePreparedIdentity();
#if defined(CROSSVI_COVER_DEBUG)
          LOG_INF("COVDBG", "EPUB thumbnails path=%s shared=%u carousel=%u", book.path.c_str(),
                  static_cast<unsigned>(thumbnails.shared), static_cast<unsigned>(thumbnails.carousel));
#endif
          finishEpubPreparation();
        }
      }
    } else if (book.format == LibraryBookFormat::Xtc || book.format == LibraryBookFormat::Xtch) {
      if (!coverPreparationXtc) {
        coverPreparationXtc.reset(new (std::nothrow) Xtc(book.path, "/.crosspoint"));
        if (!coverPreparationXtc) {
          generated = false;
          if (drawsCovers) {
            const uint8_t readyBit = static_cast<uint8_t>(1U << offset);
            coverQueueReadyMask |= readyBit;
            coverQueueShownMask |= readyBit;
            coverQueueAbsentMask |= readyBit;
          }
          ++coverQueueCursor;
          break;
        }
        coverPreparationPath = book.path;
      }
      Xtc& xtc = *coverPreparationXtc;
      const auto finishXtcPreparation = [&]() {
        if (offset == coverQueueSelected) {
          RawSourceIdentityHandoff preparedIdentity;
          if (xtc.getSourceIdentityHandoff(preparedIdentity)) {
            preparedXtcSourceIdentity = std::move(preparedIdentity);
            preparedXtc = std::move(coverPreparationXtc);
          }
        }
        coverPreparationXtc.reset();
        coverPreparationPath.clear();
      };
      const int carouselHeight =
          renderer.getDisplayHeight() == 528 ? Epub::CAROUSEL_THUMB_HEIGHT : Epub::CAROUSEL_X4_THUMB_HEIGHT;
      const int carouselWidth =
          renderer.getDisplayHeight() == 528 ? Epub::CAROUSEL_THUMB_WIDTH : Epub::CAROUSEL_X4_THUMB_WIDTH;
      Xtc::ThumbnailPreparationStatus thumbnailStatus = Xtc::ThumbnailPreparationStatus::NeedsSource;
      if (!xtc.isLoaded() && !xtc.isLoadInProgress() && !xtc.thumbnailPreparationActive()) {
        thumbnailStatus = xtc.beginThumbnailPreparation(carouselWidth, carouselHeight);
      }
      if (thumbnailStatus == Xtc::ThumbnailPreparationStatus::Ready) {
        cachedBefore = true;
        generated = true;
        finishXtcPreparation();
#if defined(CROSSVI_COVER_DEBUG)
        LOG_INF("COVDBG", "XTC thumbnail pair already valid path=%s", book.path.c_str());
#endif
      } else if (thumbnailStatus == Xtc::ThumbnailPreparationStatus::Error) {
        cachedBefore = false;
        generated = false;
        coverPreparationXtc.reset();
        coverPreparationPath.clear();
      } else {
        cachedBefore = false;
        if (!idle) return;
#if defined(CROSSVI_COVER_DEBUG)
        const uint32_t loadStart = static_cast<uint32_t>(millis());
#endif
        if (!xtc.isLoaded() && !xtc.isLoadInProgress() && !xtc.beginLoad()) {
          thumbnailStatus = Xtc::ThumbnailPreparationStatus::Error;
        } else if (!xtc.isLoaded()) {
          const Xtc::LoadStepResult load = xtc.stepLoad(4, 16U * 1024U);
          if (load == Xtc::LoadStepResult::InProgress) return;
          if (load == Xtc::LoadStepResult::Loaded && offset == coverQueueSelected) {
            RawSourceIdentityHandoff preparedIdentity;
            if (xtc.getSourceIdentityHandoff(preparedIdentity)) {
              preparedXtcSourceIdentity = std::move(preparedIdentity);
            }
          }
          if (load == Xtc::LoadStepResult::Error) thumbnailStatus = Xtc::ThumbnailPreparationStatus::Error;
        }
        if (xtc.isLoaded() && thumbnailStatus != Xtc::ThumbnailPreparationStatus::Error) {
          thumbnailStatus = xtc.thumbnailPreparationActive()
                                ? xtc.stepThumbnailPreparation(1024, 8)
                                : xtc.beginThumbnailPreparation(carouselWidth, carouselHeight);
          if (thumbnailStatus == Xtc::ThumbnailPreparationStatus::InProgress) return;
        }
        generated = thumbnailStatus == Xtc::ThumbnailPreparationStatus::Ready;
#if defined(CROSSVI_COVER_DEBUG)
        const bool xtcLoaded = xtc.isLoaded();
#endif
        if (generated) {
          finishXtcPreparation();
        } else {
          coverPreparationXtc.reset();
          coverPreparationPath.clear();
        }
#if defined(CROSSVI_COVER_DEBUG)
        LOG_INF("COVDBG", "XTC open path=%s ok=%d elapsed_ms=%u", book.path.c_str(), xtcLoaded,
                static_cast<unsigned>(static_cast<uint32_t>(millis()) - loadStart));
#endif
      }
    }
#if defined(CROSSVI_COVER_DEBUG)
    LOG_INF("COVDBG", "thumbnail done format=%u offset=%u ok=%d total_ms=%u", static_cast<unsigned>(book.format),
            static_cast<unsigned>(offset), generated, static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
#else
    (void)generated;
#endif
    // A failed attempt is still consumed so a broken cover cannot retry forever
    // and starve input. Cached covers are grouped into one refresh; a missing
    // cover still yields immediately after its bounded generation attempt.
    if (drawsCovers) {
      const uint8_t readyBit = static_cast<uint8_t>(1U << offset);
      if (!generated) coverQueueAbsentMask |= readyBit;
      if ((coverQueueShownMask & readyBit) == 0) {
        coverQueueReadyMask |= readyBit;
        coverQueueShownMask |= readyBit;
      }
    }
    ++coverQueueCursor;
    if (!cachedBefore || !generated || millis() - batchStarted >= CACHED_COVER_BATCH_BUDGET_MS) break;
  }
  if (drawsCovers && coverQueueReadyMask != 0) requestUpdate();
}

void RecentBooksActivity::processSelectedSourcePreparation() {
  if (catalogLoading() || coverPreparationEpub || coverPreparationXtc ||
      static_cast<uint32_t>(millis() - coverQueueLastInputAt) < COVER_WORK_IDLE_MS || mappedInput.wasAnyPressed() ||
      mappedInput.wasAnyReleased() || confirmBookPressCaptured || pendingNavigation != 0 || pendingTabSwitch != 0 ||
      pendingPageSwitch != 0 || pendingSearch || pendingBack || pendingTabConfirm || pendingBookConfirmRelease) {
    return;
  }

  if (!bookSelected()) {
    preparedEpub.reset();
    preparedXtc.reset();
    return;
  }

  LibraryBookRecord book;
  if (!loadVisibleBook(selectedBookIndex(), book) || book.path.empty()) return;
  if (sourcePreparationFailedPath == book.path) return;

  if (preparedEpub && preparedEpub->getPath() != book.path) preparedEpub.reset();
  if (preparedXtc && preparedXtc->getPath() != book.path) preparedXtc.reset();

  if (book.format != LibraryBookFormat::Epub) return;
  if (!preparedEpub || preparedEpub->getPath() != book.path) {
    preparedEpub.reset(new (std::nothrow) Epub(book.path, "/.crosspoint"));
    if (!preparedEpub) {
      sourcePreparationFailedPath = book.path;
      return;
    }
  }
  if (preparedEpub->hasPreparedCoreMetadata()) return;
  if (!preparedEpub->isReadingCoreMetadata() && !preparedEpub->beginCoreMetadataRead()) {
    preparedEpub.reset();
    sourcePreparationFailedPath = book.path;
    return;
  }
  BookMetadataCache::BookMetadata metadata;
  if (preparedEpub->stepCoreMetadataRead(metadata) == Epub::CoreMetadataStepResult::Error) {
    preparedEpub.reset();
    sourcePreparationFailedPath = book.path;
  }
}

int RecentBooksActivity::noticeHeight() const {
  const bool showPinHint = viewMode() == CrossPointSettings::LIBRARY_LIST && visibleBookCount() > 0;
  const bool showNotice =
      showPinHint || searchResultsTruncated[tabIndex()] || (allTab() && LIBRARY_CATALOG.isTruncated());
  return showNotice ? renderer.getLineHeight(SMALL_FONT_ID) + 4 : 0;
}

bool RecentBooksActivity::catalogLoading() const {
  if (!allTab()) return false;
  return catalogOpenPending || LIBRARY_CATALOG.isBuilding() || allSearchPending || allSearchJob.running ||
         (LIBRARY_CATALOG.isReady() && !pinnedProjectionCurrent());
}

Rect RecentBooksActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 4;
  return Rect(
      0, top, renderer.getScreenWidth(),
      std::max(0, renderer.getScreenHeight() - top - metrics.buttonHintsHeight - LIBRARY_BOTTOM_GAP - noticeHeight()));
}

void RecentBooksActivity::clearSearch(const bool preserveQuery) {
  const size_t ti = tabIndex();
  if (allTab()) cancelAllSearch();
  searchActive[ti] = false;
  searchResultsTruncated[ti] = false;
  if (!preserveQuery) searchQuery[ti].clear();
  searchResults[ti].clear();
  if (allTab()) allSearchResultGeneration = 0;
  invalidateRenderPage();
  resetCoverQueue();
}

void RecentBooksActivity::launchSearch() {
  const size_t ti = tabIndex();
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_BOOKS), searchQuery[ti],
                                              BOOK_SEARCH_QUERY_BYTES, InputType::Text, true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) applySearch(std::get<KeyboardResult>(result.data).text);
        requestUpdate();
      });
}

void RecentBooksActivity::applySearch(const std::string& query) {
  const size_t ti = tabIndex();
  invalidateRenderPage();
  const BookSearchQuery normalized = makeBookSearchQuery(query);
  if (normalized.empty()) {
    clearSearch();
    selectorIndex = 1;
    return;
  }
  searchQuery[ti] = query;
  if (allTab() && (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding())) {
    allSearchPending = true;
    searchActive[ti] = false;
    searchResults[ti].clear();
    selectorIndex = 1;
    resetCoverQueue();
    requestUpdate();
    return;
  }
  if (allTab()) allSearchPending = false;
  searchActive[ti] = true;
  searchResultsTruncated[ti] = false;
  searchResults[ti].clear();
  const size_t limit = BOOK_SEARCH_RESULT_HARD_LIMIT;
  searchResults[ti].reserve(limit);
  if (allTab()) {
    allSearchJob = {};
    allSearchJob.query = normalized;
    allSearchJob.limit = limit;
    allSearchJob.generation = LIBRARY_CATALOG.generation();
    // The visible projection already carries the persisted library order and
    // the small pinned prefix. Do not duplicate its whole index array just to
    // search it: on ESP32-C3 that would double the worst-case search heap.
    if (!pinnedProjectionCurrent()) {
      rebuildPinnedProjection();
    }
    if (!pinnedProjectionCurrent()) {
      allSearchPending = true;
      searchActive[ti] = false;
      requestUpdate();
      return;
    }
    allSearchJob.results.reserve(limit);
    allSearchJob.batch.reserve(SEARCH_RECORDS_PER_STEP);
    allSearchJob.running = true;
    selectorIndex = 1;
    resetCoverQueue();
    requestUpdate();
    return;
  }
  size_t exactCount = 0;
  const size_t count = recentBooks.size();
  LibraryBookRecord book;
  for (size_t i = 0; i < count; ++i) {
    const RecentBook& recent = recentBooks[i];
    book.path = recent.path;
    book.title = recent.title;
    book.author = recent.author;
    book.coverBmpPath = recent.coverBmpPath;
    book.format = formatForPath(recent.path);
    book.sourceSize = 0;
    BookSearchMatch rank = matchBookSearch(normalized, book.path);
    rank = std::max(rank, matchBookSearch(normalized, book.title));
    rank = std::max(rank, matchBookSearch(normalized, book.author));
    addRankedBookSearchResult(searchResults[ti], exactCount, searchResultsTruncated[ti], i, rank, limit);
  }
  selectorIndex = searchResults[ti].empty() ? 1 : controlCount();
  resetCoverQueue();
  if (bookSelected()) rememberCurrentBook();
}

void RecentBooksActivity::cancelAllSearch() {
  allSearchPending = false;
  if (!allSearchJob.running) return;
  allSearchJob = {};
  constexpr size_t allIndex = static_cast<size_t>(Tab::All);
  searchActive[allIndex] = false;
  searchResultsTruncated[allIndex] = false;
  searchResults[allIndex].clear();
  allSearchResultGeneration = 0;
}

void RecentBooksActivity::processAllSearchStep() {
  if (!allSearchJob.running) return;
  if (!LIBRARY_CATALOG.isReady() || LIBRARY_CATALOG.generation() != allSearchJob.generation) {
    const bool retryWhenReady = LIBRARY_CATALOG.isBuilding();
    const std::string query = searchQuery[static_cast<size_t>(Tab::All)];
    cancelAllSearch();
    if (retryWhenReady) {
      allSearchPending = true;
      searchQuery[static_cast<size_t>(Tab::All)] = query;
      selectorIndex = 1;
      requestUpdate();
      return;
    }
    if (LIBRARY_CATALOG.isReady()) {
      applySearch(query);
      return;
    }
    if (refreshStorageAvailability()) {
      popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
      popupTime = millis();
    }
    requestUpdate();
    return;
  }

  std::array<size_t, SEARCH_RECORDS_PER_STEP> sourceIndices{};
  size_t sourceCount = 0;
  while (sourceCount < sourceIndices.size()) {
    size_t source = static_cast<size_t>(-1);
    if (allSearchJob.pinnedCursor < pinnedSourceIndices.size()) {
      source = pinnedSourceIndices[allSearchJob.pinnedCursor++];
    } else {
      while (allSearchJob.sourceCursor < allSourceIndices.size()) {
        const size_t candidate = allSourceIndices[allSearchJob.sourceCursor++];
        if (std::find(pinnedSourceIndices.begin(), pinnedSourceIndices.end(), candidate) == pinnedSourceIndices.end()) {
          source = candidate;
          break;
        }
      }
    }
    if (source == static_cast<size_t>(-1)) break;
    sourceIndices[sourceCount++] = source;
  }
  if (sourceCount == 0) {
    allSearchJob.running = false;
  }
  if (allSearchJob.running &&
      !LIBRARY_CATALOG.loadRecords(std::span<const size_t>(sourceIndices).first(sourceCount), allSearchJob.batch)) {
    cancelAllSearch();
    if (refreshStorageAvailability()) {
      popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
      popupTime = millis();
    }
    requestUpdate();
    return;
  }

  for (size_t offset = 0; allSearchJob.running && offset < allSearchJob.batch.size(); ++offset) {
    const auto& book = allSearchJob.batch[offset];
    if (SETTINGS.hideTxtBooks && book.format == LibraryBookFormat::Text) continue;
    BookSearchMatch rank = matchBookSearch(allSearchJob.query, book.path);
    rank = std::max(rank, matchBookSearch(allSearchJob.query, book.title));
    rank = std::max(rank, matchBookSearch(allSearchJob.query, book.author));
    addRankedBookSearchResult(allSearchJob.results, allSearchJob.exactCount, allSearchJob.truncated,
                              sourceIndices[offset], rank, allSearchJob.limit);
  }
  if (allSearchJob.running &&
      (allSearchJob.pinnedCursor < pinnedSourceIndices.size() || allSearchJob.sourceCursor < allSourceIndices.size())) {
    return;
  }
  allSearchJob.running = false;
  constexpr size_t allIndex = static_cast<size_t>(Tab::All);
  searchResults[allIndex] = std::move(allSearchJob.results);
  searchResultsTruncated[allIndex] = allSearchJob.truncated;
  allSearchResultGeneration = allSearchJob.generation;
  allSearchJob = {};
  invalidateRenderPage();
  selectorIndex = searchResults[allIndex].empty() ? 1 : controlCount();
  resetCoverQueue();
  if (restoreReturnAfterSearch) {
    restoreReturnAfterSearch = false;
    restoreRememberedBook(true);
  }
  if (bookSelected()) rememberCurrentBook();
  requestUpdate();
}

void RecentBooksActivity::onEnter() {
#if defined(ENABLE_SERIAL_LOG)
  libraryTraceActivityStartedAt = static_cast<uint32_t>(millis());
  libraryTraceTabStartedAt = libraryTraceActivityStartedAt;
  libraryTracePhaseStartedAt = libraryTraceActivityStartedAt;
  libraryTraceOrderStartedAt = libraryTraceActivityStartedAt;
  libraryTraceOrderPhaseStartedAt = libraryTraceActivityStartedAt;
  libraryTraceNextNavigationId = 0;
  libraryTraceQueuedFirstId = 0;
  libraryTraceQueuedLastId = 0;
  libraryTraceQueuedAt = 0;
  libraryTraceVisibleFirstId = 0;
  libraryTraceVisibleLastId = 0;
  libraryTraceVisibleInputAt = 0;
  libraryTraceCatalogPhase = LIBRARY_CATALOG.phase();
  libraryTraceOrderPhase = LIBRARY_CATALOG.orderPhase();
  libraryTraceFirstVisiblePending = true;
#endif
  Activity::onEnter();
#if defined(CROSSVI_COVER_DEBUG)
  LOG_INF("COVDBG", "cover diagnostic firmware enabled");
#endif
  invalidateRenderPage();
  coverQueueLastInputAt = static_cast<uint32_t>(millis());
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t storageProbeStartedAt = static_cast<uint32_t>(millis());
#endif
  storageAvailable = Storage.probeMedia();
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t storageProbeElapsed = static_cast<uint32_t>(millis()) - storageProbeStartedAt;
#endif
  recentBooks.clear();
  if (storageAvailable) {
    // Keep activity entry independent of the number and location of recent or
    // pinned books. pruneMissing() performs one FAT lookup per path (up to 22
    // here), which can block the main loop for seconds on a physical SD card
    // before the first button edge can be sampled. Normal add/delete/move
    // workflows already maintain the store; Home verifies external removals
    // one path at a time after its first frame.
    loadRecentBooks();
  } else {
    LIBRARY_CATALOG.cancel();
    resetCoverQueue();
  }
  tab = Tab::Recent;
  confirmPressSeen = false;
  confirmLongHandled = false;
  confirmTabHandled = false;
  confirmBookPressCaptured = false;
  pendingBookConfirmRelease = false;
  pendingTabConfirm = false;
  suppressPopupConfirmRelease = false;
  pendingPopupNavigation = 0;
  pendingPopupConfirmRelease = false;
  pendingPopupBackRelease = false;
  redrawBookActionsBackground = false;
  preserveTabFocus = false;
  pendingNavigation = 0;
  pendingTabSwitch = 0;
  pendingPageSwitch = 0;
  pendingSearch = false;
  pendingBack = false;
  holdUp.reset();
  holdDown.reset();
  holdLeft.reset();
  holdRight.reset();
  holdBack.reset();
  navigationReleaseGuard.reset();
  pinnedSourceIndices.clear();
  allSourceIndices.clear();
  allSourceIndicesValid = false;
  pinnedProjectionGeneration = 0;
  pinnedProjectionValid = false;
  catalogOpenPending = false;
  restoreReturnAfterSearch = false;
  selectorIndex = 0;
  if (pendingReturnState.has_value()) {
    const YourBooksReturnState state = std::move(*pendingReturnState);
    pendingReturnState.reset();
    tab = state.tab == static_cast<uint8_t>(Tab::All) ? Tab::All : Tab::Recent;
    const size_t ti = tabIndex();
    rememberedBookIndex[ti] = state.selectedIndex;
    rememberedBookPath[ti] = state.selectedPath;
    searchQuery[ti] = state.searchQuery;
    preserveTabFocus = false;
    if (allTab()) {
      catalogOpenPending = true;
      if (!searchQuery[ti].empty()) restoreReturnAfterSearch = true;
    } else if (!searchQuery[ti].empty()) {
      applySearch(searchQuery[ti]);
      restoreRememberedBook(true);
    } else {
      restoreRememberedBook(true);
    }
  }
#if defined(ENABLE_SERIAL_LOG)
  libraryTraceTabStartedAt = libraryTraceActivityStartedAt;
  LOG_DBG("LIBT", "enter_ready tab=%s storage=%u recent=%u catalog_phase=%s catalog_count=%u probe_ms=%u total_ms=%u",
          libraryTabName(allTab()), static_cast<unsigned>(storageAvailable), static_cast<unsigned>(recentBooks.size()),
          catalogPhaseName(LIBRARY_CATALOG.phase()), static_cast<unsigned>(LIBRARY_CATALOG.count()),
          static_cast<unsigned>(storageProbeElapsed),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - libraryTraceActivityStartedAt));
#endif
  const bool warmAllBooks = catalogOpenPending && LIBRARY_CATALOG.isReady() && !LIBRARY_CATALOG.isOrderBuilding();
  if (!warmAllBooks) requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  resetCoverQueue();
  cancelAllSearch();
  LIBRARY_CATALOG.cancel();
  recentBooks.clear();
  renderPage.clear();
  invalidateRenderPage();
  freeGridSnapshot();
  holdUp.reset();
  holdDown.reset();
  holdLeft.reset();
  holdRight.reset();
  holdBack.reset();
  pinnedSourceIndices.clear();
  allSourceIndices.clear();
  allSourceIndicesValid = false;
  pinnedProjectionValid = false;
  pendingReturnState.reset();
  suppressPopupConfirmRelease = false;
  pendingPopupNavigation = 0;
  pendingPopupConfirmRelease = false;
  pendingPopupBackRelease = false;
  redrawBookActionsBackground = false;
  restoreReturnAfterSearch = false;
  for (auto& results : searchResults) results.clear();
}

void RecentBooksActivity::onPause() { cancelCoverPreparation(); }

bool RecentBooksActivity::skipLoopDelay() {
  return (coverPreparationEpub &&
          (coverPreparationEpub->isReadingCoreMetadata() || coverPreparationEpub->thumbnailPreparationActive())) ||
         (coverPreparationXtc &&
          (coverPreparationXtc->isLoadInProgress() || coverPreparationXtc->thumbnailPreparationActive())) ||
         (preparedEpub && preparedEpub->isReadingCoreMetadata()) || LIBRARY_CATALOG.isOrderBuilding() ||
         LIBRARY_CATALOG.isBuilding();
}

void RecentBooksActivity::loop() {
  // Capture navigation before waiting for e-ink rendering. A blocking lock here
  // would stop gpio.update(), so quick presses made during refresh would be
  // lost instead of being applied after the refresh completes.
  queueNavigationInput();
  RenderLock lock(std::try_to_lock);
  if (!lock.ownsLock()) return;

  // The render task reads the same catalog/search vectors and may load the
  // active catalog. Serialize those operations with the render task, while
  // keeping the input edge path above non-blocking.
  applyPendingNavigation();

  if (optionPopup.isActive()) {
    if (pendingPopupNavigation != 0) {
      const int delta = pendingPopupNavigation;
      pendingPopupNavigation = 0;
      optionPopup.moveSelection(delta, [this] { requestUpdate(); });
    }
    if (pendingPopupBackRelease) {
      pendingPopupBackRelease = false;
      pendingPopupConfirmRelease = false;
      optionPopup.dismiss([this] { requestUpdate(); });
      return;
    }
    if (pendingPopupConfirmRelease) {
      pendingPopupConfirmRelease = false;
      optionPopup.selectCurrent([this] { requestUpdate(); });
    }
    return;
  }

  if (pendingSearch) {
    pendingSearch = false;
    pendingBack = false;
    lock.unlock();
    launchSearch();
    return;
  }

  if (pendingBack) {
    pendingBack = false;
    // A cooperative All search is cancellable from the library itself. Keep
    // the query so the next held-Back search can reopen it, but publish the
    // unfiltered catalog immediately instead of letting the old job finish
    // and replace the screen after the user has already cancelled it.
    if (allTab() && (allSearchJob.running || allSearchPending)) {
      cancelAllSearch();
      allSearchPending = false;
      searchActive[static_cast<size_t>(Tab::All)] = false;
      searchResults[static_cast<size_t>(Tab::All)].clear();
      searchResultsTruncated[static_cast<size_t>(Tab::All)] = false;
      allSearchResultGeneration = 0;
      selectorIndex = 0;
      invalidateRenderPage();
      resetCoverQueue();
      requestUpdate();
      return;
    }
    if (bookSelected()) {
      selectorIndex = 0;
      requestUpdate();
    } else {
      lock.unlock();
      onGoHome();
    }
    return;
  }

  if (pendingTabConfirm) {
    pendingTabConfirm = false;
    confirmPressSeen = false;
    selectTab(allTab() ? Tab::Recent : Tab::All);
    return;
  }

  if (pendingBookConfirmRelease) {
    pendingBookConfirmRelease = false;
    if (confirmBookPressCaptured) {
      confirmBookPressCaptured = false;
      const bool longPress = mappedInput.getHeldTime(MappedInputManager::Button::Confirm) >= LONG_PRESS_MS;
      confirmPressSeen = false;
      if (longPress && bookSelected()) {
        confirmLongHandled = true;
        suppressPopupConfirmRelease = false;
        showBookActions(selectedBookIndex());
      } else if (!longPress && bookSelected()) {
        LibraryBookRecord book;
        if (loadVisibleBook(selectedBookIndex(), book)) {
          const std::string path = book.path;
          captureReaderReturnContext(book);
          lock.unlock();
          openSelectedBook(path);
        }
      }
      return;
    }
  }

  // Tab selection is a direct action. Handle it before catalog I/O or cover
  // work, and swallow the matching release so the new tab is not toggled
  // straight back when the user lets Confirm go.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen = true;
    confirmLongHandled = false;
    if (tabSelected()) {
      selectTab(allTab() ? Tab::Recent : Tab::All);
      confirmTabHandled = true;
      confirmPressSeen = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmTabHandled) {
    confirmTabHandled = false;
    return;
  }

  if (catalogOpenPending && allTab() && !LIBRARY_CATALOG.isBuilding() && !LIBRARY_CATALOG.isOrderBuilding()) {
    catalogOpenPending = false;
#if defined(ENABLE_SERIAL_LOG)
    const uint32_t catalogSetupStartedAt = static_cast<uint32_t>(millis());
#endif
    if (!refreshStorageAvailability()) {
#if defined(ENABLE_SERIAL_LOG)
      LOG_DBG("LIBT", "catalog_open ok=0 storage=0 phase=%s count=%u setup_ms=%u tab_ms=%u",
              catalogPhaseName(LIBRARY_CATALOG.phase()), static_cast<unsigned>(LIBRARY_CATALOG.count()),
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - catalogSetupStartedAt),
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - libraryTraceTabStartedAt));
#endif
      requestUpdate();
      return;
    }
#if defined(ENABLE_SERIAL_LOG)
    const uint32_t catalogOpenStartedAt = static_cast<uint32_t>(millis());
#endif
    const bool catalogOpened = LIBRARY_CATALOG.open();
    if (!catalogOpened) {
      const bool stillAvailable = refreshStorageAvailability();
      LIBRARY_CATALOG.consumeLastBuildFailed();
      if (stillAvailable) {
        popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
        popupTime = millis();
      }
      if (LIBRARY_CATALOG.isReady()) {
        rebuildPinnedProjection();
        invalidateRenderPage();
        resetCoverQueue();
      }
    } else if (LIBRARY_CATALOG.isReady()) {
      rebuildPinnedProjection();
      invalidateRenderPage();
      resetCoverQueue();
      if (!LIBRARY_CATALOG.isOrderBuilding() && !preserveTabFocus && !searchActive[tabIndex()]) {
        if (!searchQuery[tabIndex()].empty()) {
          allSearchPending = true;
          restoreReturnAfterSearch = true;
        } else {
          restoreRememberedBook(true);
        }
      }
    }
#if defined(ENABLE_SERIAL_LOG)
    const uint32_t catalogOpenedAt = static_cast<uint32_t>(millis());
    LOG_DBG("LIBT", "catalog_open ok=%u storage=1 phase=%s count=%u open_ms=%u setup_ms=%u tab_ms=%u",
            static_cast<unsigned>(catalogOpened), catalogPhaseName(LIBRARY_CATALOG.phase()),
            static_cast<unsigned>(LIBRARY_CATALOG.count()),
            static_cast<unsigned>(catalogOpenedAt - catalogOpenStartedAt),
            static_cast<unsigned>(catalogOpenedAt - catalogSetupStartedAt),
            static_cast<unsigned>(catalogOpenedAt - libraryTraceTabStartedAt));
    traceCatalogState("open");
#endif
    if (!LIBRARY_CATALOG.isBuilding() && !LIBRARY_CATALOG.isOrderBuilding()) requestUpdate();
    return;
  }

  constexpr size_t allIndex = static_cast<size_t>(Tab::All);
  if (allTab() && LIBRARY_CATALOG.isReady() && !LIBRARY_CATALOG.isOrderBuilding() &&
      (!pinnedProjectionCurrent() || pinnedProjectionGeneration != LIBRARY_CATALOG.generation())) {
    rememberCurrentBook();
    rebuildPinnedProjection();
    // A missing order sidecar is built cooperatively. Keep the already-visible
    // loading frame until that derived index is ready instead of refreshing a
    // second loading-only frame just to change INDEXING to SORTING.
    if (!LIBRARY_CATALOG.isOrderBuilding()) {
      if (searchActive[allIndex]) {
        restoreReturnAfterSearch = true;
      } else {
        restoreRememberedBook(true);
      }
      invalidateRenderPage();
      resetCoverQueue();
      requestUpdate();
    }
  }
  if (searchActive[allIndex] && !allSearchJob.running && allSearchResultGeneration != LIBRARY_CATALOG.generation()) {
    searchActive[allIndex] = false;
    searchResultsTruncated[allIndex] = false;
    searchResults[allIndex].clear();
    allSearchResultGeneration = 0;
    allSearchPending = true;
    invalidateRenderPage();
    if (allTab()) selectorIndex = 1;
    resetCoverQueue();
    requestUpdate();
  }
  if (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding()) {
    const unsigned long started = millis();
    for (uint8_t step = 0;
         step < CATALOG_STEPS_PER_LOOP && (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding()); ++step) {
      LIBRARY_CATALOG.step();
#if defined(ENABLE_SERIAL_LOG)
      traceCatalogState("step");
#endif
      if (millis() - started >= CATALOG_LOOP_BUDGET_MS) break;
    }
  }
  if (LIBRARY_CATALOG.consumeLastBuildFailed()) {
    if (refreshStorageAvailability()) {
      popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
      popupTime = millis();
    } else {
      cancelAllSearch();
      allSearchPending = false;
    }
    requestUpdate();
  }
  if (allSearchPending && allTab() && !LIBRARY_CATALOG.isBuilding() && !LIBRARY_CATALOG.isOrderBuilding()) {
    if (LIBRARY_CATALOG.phase() == LibraryCatalogStore::Phase::Ready) {
      const std::string query = searchQuery[tabIndex()];
      allSearchPending = false;
      applySearch(query);
    } else {
      allSearchPending = false;
      if (refreshStorageAvailability()) {
        popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
        popupTime = millis();
      }
      requestUpdate();
    }
  }
  processAllSearchStep();

  if (popupMessage != StrId::STR_NONE_OPT) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popupMessage = StrId::STR_NONE_OPT;
      requestUpdate();
    }
    return;
  }
  processCoverQueue();
  processSelectedSourcePreparation();

  const size_t availableFocus = controlCount() + visibleBookCount();
  if (availableFocus > 0 && selectorIndex >= availableFocus) selectorIndex = availableFocus - 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen = true;
    confirmLongHandled = false;
  }
  if (confirmPressSeen && !confirmLongHandled && bookSelected() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Confirm) >= LONG_PRESS_MS) {
    confirmLongHandled = true;
    confirmBookPressCaptured = false;
    suppressPopupConfirmRelease = true;
    showBookActions(selectedBookIndex());
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmTabHandled) {
      confirmTabHandled = false;
      return;
    }
    if (confirmLongHandled) {
      confirmPressSeen = false;
      confirmLongHandled = false;
      suppressPopupConfirmRelease = false;
      return;
    }
    const bool accept = confirmPressSeen;
    confirmPressSeen = false;
    confirmBookPressCaptured = false;
    if (!accept) return;
    if (tabSelected()) {
      selectTab(allTab() ? Tab::Recent : Tab::All);
    } else if (bookSelected()) {
      LibraryBookRecord book;
      if (loadVisibleBook(selectedBookIndex(), book)) {
        const std::string path = book.path;
        captureReaderReturnContext(book);
        lock.unlock();
        openSelectedBook(path);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (bookSelected()) {
      selectorIndex = 0;
      requestUpdate();
    } else {
      lock.unlock();
      onGoHome();
    }
    return;
  }
}

void RecentBooksActivity::showBookActions(const size_t visibleIndex) {
  cancelCoverPreparation();
  LibraryBookRecord selected;
  if (!loadVisibleBook(visibleIndex, selected)) return;
  const bool inRecent = std::any_of(RECENT_BOOKS.getBooks().begin(), RECENT_BOOKS.getBooks().end(),
                                    [&selected](const RecentBook& book) { return book.path == selected.path; });
  std::string completionCache;
  BookReadingStats completionStats;
  GlobalReadingStats completionGlobal;
  const bool completionAvailable = loadCompletionState(selected, completionCache, completionStats, completionGlobal);
  std::vector<BookAction> actions = {BookAction::Open, BookAction::Stats, BookAction::Saved, BookAction::ClearCache};
  std::vector<std::string> options = {tr(STR_OPEN_BOOK), tr(STR_READING_STATS), tr(STR_BOOKMARKS_AND_HIGHLIGHTS),
                                      tr(STR_DELETE_CACHE)};
  if (completionAvailable) {
    actions.push_back(BookAction::Completion);
    options.push_back(
        I18N.get(completionStats.isCompleted ? StrId::STR_MARK_BOOK_UNREAD : StrId::STR_MARK_BOOK_FINISHED));
  }
  actions.push_back(BookAction::Pin);
  options.push_back(RECENT_BOOKS.isPinned(selected.path) ? tr(STR_UNPIN_BOOK) : tr(STR_PIN_BOOK));
  if (!allTab() && inRecent) {
    actions.push_back(BookAction::RemoveRecent);
    options.push_back(tr(STR_REMOVE_FROM_RECENTS));
  }
  actions.push_back(BookAction::Delete);
  options.push_back(tr(STR_DELETE));
  optionPopup.show(
      StrId::STR_BOOK_ACTIONS, std::move(options), 0,
      [this, visibleIndex, selected, actions = std::move(actions), completionStats](const int option) {
        if (option < 0 || static_cast<size_t>(option) >= actions.size()) return;
        switch (actions[static_cast<size_t>(option)]) {
          case BookAction::Open:
            captureReaderReturnContext(selected);
            openSelectedBook(selected.path);
            return;
          case BookAction::Stats: {
            ReadingStatsPresentation presentation;
            if (!loadBookStatsPresentation({selected.path, selected.title, selected.author, selected.coverBmpPath},
                                           presentation)) {
              popupMessage = StrId::STR_STATS_UNAVAILABLE;
              popupTime = millis();
              requestUpdate();
              return;
            }
            const std::string title = selected.title.empty() ? selected.path : selected.title;
            startActivityForResult(
                std::make_unique<ReadingStatsActivity>(renderer, mappedInput, title, std::move(presentation),
                                                       ReadingStatsActivity::Page::Book, false, false, selected.path),
                [this](const ActivityResult&) { requestUpdate(); });
            return;
          }
          case BookAction::Saved:
            startActivityForResult(
                std::make_unique<BookSavedItemsActivity>(renderer, mappedInput, selected.path, selected.title,
                                                         selected.author, savedItemsKind(selected.path)),
                [this](const ActivityResult&) { requestUpdate(); });
            return;
          case BookAction::ClearCache: {
            const std::string cachePath = bookCachePath(selected.path);
            popupMessage = !cachePath.empty() && clearBookCacheDirectoryPreservingUserState(cachePath)
                               ? StrId::STR_BOOK_CACHE_CLEARED
                               : StrId::STR_CLEAR_CACHE_FAILED;
            popupTime = millis();
            requestUpdate();
            return;
          }
          case BookAction::Completion:
            popupMessage =
                setBookCompletion(selected, !completionStats.isCompleted)
                    ? (completionStats.isCompleted ? StrId::STR_BOOK_MARKED_UNREAD : StrId::STR_BOOK_MARKED_FINISHED)
                    : StrId::STR_ERROR_GENERAL_FAILURE;
            popupTime = millis();
            requestUpdate();
            return;
          case BookAction::Pin: {
            rememberedBookIndex[tabIndex()] = visibleIndex;
            rememberedBookPath[tabIndex()] = selected.path;
            const auto result = RECENT_BOOKS.togglePin(selected.path);
            popupMessage = result == RecentBooksStore::PinResult::Pinned         ? StrId::STR_BOOK_PINNED
                           : result == RecentBooksStore::PinResult::Unpinned     ? StrId::STR_BOOK_UNPINNED
                           : result == RecentBooksStore::PinResult::LimitReached ? StrId::STR_PIN_LIMIT_REACHED
                                                                                 : StrId::STR_ERROR_GENERAL_FAILURE;
            popupTime = millis();
            if (!allTab()) {
              rememberedBookIndex[tabIndex()] = visibleIndex;
              rememberedBookPath[tabIndex()] = selected.path;
            }
            const bool rebuildSearch = !allTab() && searchActive[tabIndex()];
            const std::string activeQuery = rebuildSearch ? searchQuery[tabIndex()] : std::string{};
            loadRecentBooks();
            if (rebuildSearch) applySearch(activeQuery);
            if (allTab()) {
              rebuildPinnedProjection();
              // Pinning changes the visible order without changing the catalog
              // generation. Drop the cached page and cover queue so the next frame
              // cannot open/render the previous order.
              invalidateRenderPage();
              resetCoverQueue();
            }
            rememberedBookPath[tabIndex()] = selected.path;
            restoreRememberedBook(true);
            requestUpdate();
            return;
          }
          case BookAction::RemoveRecent:
            promptRemoveBook(selected.path, selected.title);
            return;
          case BookAction::Delete:
            promptDeleteBook(visibleIndex, selected.path, selected.title);
            return;
        }
      });
  requestUpdate();
}

void RecentBooksActivity::promptDeleteBook(const size_t visibleIndex, const std::string& path,
                                           const std::string& title) {
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_CONFIRM_DELETE_BOOK),
                                                                title, tr(STR_NO), tr(STR_YES)),
                         [this, visibleIndex, path](const ActivityResult& result) {
                           if (result.isCancelled) {
                             redrawBookActionsBackground = true;
                             showBookActions(visibleIndex);
                             return;
                           }
                           const size_t ti = tabIndex();
                           const size_t countBeforeDelete = visibleBookCount();
                           std::string successorPath;
                           LibraryBookRecord successor;
                           if (visibleIndex + 1 < countBeforeDelete && loadVisibleBook(visibleIndex + 1, successor)) {
                             successorPath = successor.path;
                           } else if (visibleIndex > 0 && loadVisibleBook(visibleIndex - 1, successor)) {
                             successorPath = successor.path;
                           }
                           const bool rebuildSearch = searchActive[ti];
                           const std::string activeQuery = rebuildSearch ? searchQuery[ti] : std::string{};
                           if (!canDeleteOrRelocateBookFile(path) || !Storage.remove(path.c_str())) {
                             popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
                             popupTime = millis();
                             requestUpdate();
                             return;
                           }
                           removeBookUserStateAfterDelete(path, true);
                           if (RECENT_BOOKS.isPinned(path)) RECENT_BOOKS.togglePin(path);
                           RECENT_BOOKS.removeByPath(path);
                           rememberedBookPath[ti] = successorPath;
                           rememberedBookIndex[ti] = visibleIndex;
                           loadRecentBooks();
                           if (!allTab() && rebuildSearch) applySearch(activeQuery);
                           if (allTab()) {
                             pinnedProjectionValid = false;
                             catalogOpenPending = true;
                             preserveTabFocus = false;
                             restoreReturnAfterSearch = rebuildSearch;
                             selectorIndex = 0;
                           } else {
                             restoreRememberedBook(true);
                           }
                           requestUpdate(true);
                         });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      [this, path](const ActivityResult& result) {
        if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
          const bool rebuildSearch = searchActive[tabIndex()];
          const std::string activeQuery = rebuildSearch ? searchQuery[tabIndex()] : std::string{};
          const size_t removedIndex = rememberedBookIndex[tabIndex()];
          loadRecentBooks();
          if (rebuildSearch) applySearch(activeQuery);
          rememberedBookPath[tabIndex()].clear();
          rememberedBookIndex[tabIndex()] = LibraryGridModel::clampIndex(removedIndex, visibleBookCount());
          restoreRememberedBook();
          requestUpdate(true);
        }
      });
}

void RecentBooksActivity::render(RenderLock&&) {
#if defined(ENABLE_SERIAL_LOG)
  const uint32_t renderStartedAt = static_cast<uint32_t>(millis());
  const uint32_t navigationFirstId = libraryTraceVisibleFirstId;
  const uint32_t navigationLastId = libraryTraceVisibleLastId;
  const uint32_t navigationInputAt = libraryTraceVisibleInputAt;
  bool renderPageCacheHit = false;
  int gridSnapshotHit = -1;
  bool coverBatchRendered = false;
#endif
  if (renderBookLoadingOverlay()) return;
  const bool redrawPopupBackground = optionPopup.isActive() && redrawBookActionsBackground;
  if (optionPopup.isActive() && !redrawPopupBackground && optionPopup.processRender(renderer, mappedInput)) return;
  redrawBookActionsBackground = false;
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const size_t bookCount = visibleBookCount();
  char searchTitle[BOOK_SEARCH_QUERY_BYTES + 96]{};
  char pageLabel[16]{};
  const char* header = tr(STR_MENU_RECENT_BOOKS);
  if (searchActive[tabIndex()]) {
    std::snprintf(searchTitle, sizeof(searchTitle), tr(STR_SEARCH_RESULTS_FORMAT), searchQuery[tabIndex()].c_str());
    header = searchTitle;
  }
  if (bookCount > 0) {
    const size_t anchor = bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()];
    std::snprintf(pageLabel, sizeof(pageLabel), "%u/%u",
                  static_cast<unsigned>(LibraryGridModel::pageNumber(anchor, bookCount, pageCapacity())),
                  static_cast<unsigned>(LibraryGridModel::pageCount(bookCount, pageCapacity())));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header,
                 pageLabel[0] == '\0' ? nullptr : pageLabel);

  const int tabY = metrics.topPadding + metrics.headerHeight;
  const std::array<TabInfo, 2> tabs = {
      {{tr(STR_LIBRARY_RECENT_TAB), tab == Tab::Recent}, {tr(STR_LIBRARY_ALL_TAB), tab == Tab::All}}};
  GUI.drawTabBar(renderer, Rect{0, tabY, pageWidth, metrics.tabBarHeight}, tabs, tabSelected());

  const Rect content = contentRect();
  const int contentTop = content.y;
  const int contentHeight = content.height;
  const bool showCatalogLoading = catalogLoading();
  const bool showPinHint =
      !showCatalogLoading && viewMode() == CrossPointSettings::LIBRARY_LIST && visibleBookCount() > 0;
  if (showCatalogLoading) {
    const char* loadingText = LIBRARY_CATALOG.isOrderBuilding() ? tr(STR_LIBRARY_SORTING) : tr(STR_LIBRARY_INDEXING);
    const std::string loadingLabel = renderer.truncatedText(UI_12_FONT_ID, loadingText, std::max(1, pageWidth - 48));
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, loadingLabel.c_str());
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int boxWidth = std::min(pageWidth - 24, labelWidth + 24);
    const int boxHeight = lineHeight + 16;
    const int boxX = (pageWidth - boxWidth) / 2;
    const int boxY = contentTop + (contentHeight - boxHeight) / 2;
    renderer.fillRoundedRect(boxX, boxY, boxWidth, boxHeight, 6, Color::Black);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8, loadingLabel.c_str(), false);
  } else if (bookCount == 0) {
    const char* message = catalogOpenPending ||
                                  (allTab() && (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding())) ||
                                  (allTab() && LIBRARY_CATALOG.isReady() && !pinnedProjectionCurrent())
                              ? tr(STR_LOADING_POPUP)
                          : allSearchJob.running     ? tr(STR_LOADING_POPUP)
                          : searchActive[tabIndex()] ? tr(STR_NO_SEARCH_RESULTS)
                          : allTab()                 ? tr(STR_NO_LIBRARY_BOOKS)
                                                     : tr(STR_NO_RECENT_BOOKS);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, message);
  } else if (viewMode() == CrossPointSettings::LIBRARY_COVERS) {
    const size_t capacity = pageCapacity();
    const size_t selected =
        LibraryGridModel::clampIndex(bookSelected() ? selectedBookIndex() : rememberedBookIndex[tabIndex()], bookCount);
    const size_t pageStart = LibraryGridModel::pageStart(selected, bookCount, capacity);
    const size_t pageCount = std::min(capacity, bookCount - pageStart);
    const uint8_t settledCoverMask =
        pageCount >= 8 ? UINT8_MAX : static_cast<uint8_t>((static_cast<uint16_t>(1U) << pageCount) - 1U);
    const Rect gridRect{0, contentTop, pageWidth, contentHeight};
#if defined(ENABLE_SERIAL_LOG)
    renderPageCacheHit =
        renderPageValid && renderPageTab == tab && renderPageStart == pageStart && renderPageCount == pageCount;
#endif
    loadRenderPage(pageStart, pageCount);
    const bool restoredGridSnapshot = restoreGridSnapshot(gridRect, pageStart, pageCount, gridMode());
#if defined(ENABLE_SERIAL_LOG)
    gridSnapshotHit = restoredGridSnapshot ? 1 : 0;
#endif
    if (!restoredGridSnapshot) {
      // Cached covers are bounded 1-bit BMPs. Read the whole visible page into
      // one framebuffer so six books cost one panel refresh, not six sequential
      // refreshes after the screen is already visible. Missing caches remain
      // cheap placeholders and are generated later by the cooperative queue.
      const bool queueStarted = coverQueuePageStart == pageStart;
      coverQueueShownMask = LibraryGridView::drawStatic(renderer, gridRect, renderPage, gridMode(), true);
      coverQueueShownMask |= coverQueueAbsentMask;
      for (size_t offset = 0; offset < renderPage.size() && offset < 8; ++offset) {
        const uint8_t coverBit = static_cast<uint8_t>(1U << offset);
        const bool noCover = (coverQueueAbsentMask & coverBit) != 0 || renderPage[offset].coverBmpPath.empty() ||
                             ((coverQueueShownMask & coverBit) == 0 && hasCachedNoCoverMarker(renderPage[offset]));
        if (noCover) {
          coverQueueAbsentMask |= coverBit;
          if (!renderPage[offset].coverBmpPath.empty()) {
            renderPage[offset].coverBmpPath.clear();
            if (!allTab()) {
              const size_t recentIndex = sourceIndex(pageStart + offset);
              if (recentIndex < recentBooks.size()) recentBooks[recentIndex].coverBmpPath.clear();
            }
          }
          coverQueueShownMask |= coverBit;
        }
        if (renderPage[offset].format == LibraryBookFormat::Text ||
            renderPage[offset].format == LibraryBookFormat::Markdown) {
          coverQueueShownMask |= coverBit;
        }
      }
      coverQueueReadyMask = 0;
      storeGridSnapshot(gridRect, pageStart, pageCount, gridMode());
      if (!queueStarted) {
        coverQueuePageStart = pageStart;
        coverQueueCursor = 0;
        coverQueueSelected = selected - pageStart;
      }
    } else if (!showCatalogLoading && coverQueuePageStart == pageStart && coverQueueReadyMask != 0) {
      // Draw every cache hit collected within the loop budget, then publish one
      // framebuffer refresh and one updated page snapshot for the whole batch.
      const uint8_t readyMask = coverQueueReadyMask;
      coverQueueReadyMask = 0;
#if defined(ENABLE_SERIAL_LOG)
      coverBatchRendered = true;
#endif
      for (size_t offset = 0; offset < pageCount; ++offset) {
        if ((readyMask & (1U << offset)) != 0) {
          LibraryGridView::drawCoverAt(renderer, gridRect, renderPage, offset, gridMode());
        }
      }
      storeGridSnapshot(gridRect, pageStart, pageCount, gridMode());
    }
    const bool coversLoading = !showCatalogLoading && coverQueuePageStart == pageStart &&
                               (coverQueueShownMask & settledCoverMask) != settledCoverMask;
    if (coversLoading) {
      const int maxTextWidth = std::max(1, pageWidth - 48);
      const std::string loadingLabel = renderer.truncatedText(UI_12_FONT_ID, tr(STR_LOADING_COVER), maxTextWidth - 24);
      const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, loadingLabel.c_str());
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      const int boxWidth = std::min(pageWidth - 24, labelWidth + 24);
      const int boxHeight = lineHeight + 16;
      const int boxX = (pageWidth - boxWidth) / 2;
      const int boxY = contentTop + (contentHeight - boxHeight) / 2;
      renderer.fillRoundedRect(boxX, boxY, boxWidth, boxHeight, 6, Color::Black);
      renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8, loadingLabel.c_str(), false);
    }
    LibraryGridView::drawSelection(
        renderer, gridRect, renderPage,
        bookSelected() && selected >= pageStart ? selected - pageStart : static_cast<size_t>(-1), gridMode());
  } else {
    const int selected = bookSelected() ? static_cast<int>(selectedBookIndex()) : -1;
    constexpr size_t LIST_PAGE_ITEMS = LibraryGridModel::LIST_PAGE_SIZE;
    const int rowHeight = UITheme::getInstance().getMetrics().listWithSubtitleRowHeight;
    const int listHeight = std::min(contentHeight, static_cast<int>(LIST_PAGE_ITEMS * rowHeight));
    const int pageItems = static_cast<int>(LIST_PAGE_ITEMS);
    const size_t anchor = LibraryGridModel::clampIndex(
        selected >= 0 ? static_cast<size_t>(selected) : rememberedBookIndex[tabIndex()], bookCount);
    const size_t pageStart = anchor / static_cast<size_t>(pageItems) * static_cast<size_t>(pageItems);
    const size_t pageCount = std::min(static_cast<size_t>(pageItems), bookCount - pageStart);
#if defined(ENABLE_SERIAL_LOG)
    renderPageCacheHit =
        renderPageValid && renderPageTab == tab && renderPageStart == pageStart && renderPageCount == pageCount;
#endif
    loadRenderPage(pageStart, pageCount);
    if (coverCachesRequested() && coverQueuePageStart != pageStart) {
      coverQueuePageStart = pageStart;
      coverQueueCursor = 0;
      coverQueueSelected = anchor - pageStart;
      coverQueueReadyMask = 0;
    }
    const auto pageBook = [this, pageStart](const int index) -> const LibraryBookRecord* {
      if (index < 0 || static_cast<size_t>(index) < pageStart) return nullptr;
      const size_t local = static_cast<size_t>(index) - pageStart;
      return local < renderPage.size() ? &renderPage[local] : nullptr;
    };
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, listHeight}, static_cast<int>(bookCount), selected,
        [&pageBook](const int index) {
          const LibraryBookRecord* book = pageBook(index);
          return book ? book->title : std::string{};
        },
        [&pageBook](const int index) {
          const LibraryBookRecord* book = pageBook(index);
          if (!book) return std::string{};
          return !book->author.empty() ? book->author : formatLabel(book->format);
        },
        [&pageBook](const int index) {
          const LibraryBookRecord* book = pageBook(index);
          return book ? UITheme::getFileIcon(book->path) : UIIcon::None;
        },
        nullptr, false, nullptr,
        [&pageBook](const int index) {
          const LibraryBookRecord* book = pageBook(index);
          return book && book->pinned;
        },
        tabSelected() ? static_cast<int>(anchor) : -1);
  }

  const int footerY = contentTop + contentHeight;
  if (!showCatalogLoading && searchResultsTruncated[tabIndex()]) {
    const std::string message =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_SEARCH_MORE_RESULTS), pageWidth - metrics.contentSidePadding * 2);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, footerY, message.c_str());
  } else if (!showCatalogLoading && allTab() && LIBRARY_CATALOG.isTruncated()) {
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, footerY, tr(STR_LIBRARY_LIMIT_REACHED));
  } else if (showPinHint) {
    const int width = pageWidth - metrics.contentSidePadding * 2;
    const std::string hint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_HOLD_SELECT_PIN_HINT), width);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, footerY, hint.c_str());
  }

  const char* confirm = tabSelected()    ? (allTab() ? tr(STR_LIBRARY_RECENT_TAB) : tr(STR_LIBRARY_ALL_TAB))
                        : bookSelected() ? tr(STR_OPEN)
                                         : "";
  const char* back = tabSelected() ? tr(STR_HOME) : (allTab() ? tr(STR_LIBRARY_ALL_TAB) : tr(STR_LIBRARY_RECENT_TAB));
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (redrawPopupBackground && optionPopup.processRender(renderer, mappedInput)) {
    return;
  }
  if (popupMessage != StrId::STR_NONE_OPT) {
    GUI.drawPopup(renderer, I18N.get(popupMessage));
  } else {
#if defined(ENABLE_SERIAL_LOG)
    const uint32_t displayStartedAt = static_cast<uint32_t>(millis());
#endif
    renderer.displayBuffer();
#if defined(ENABLE_SERIAL_LOG)
    const uint32_t visibleAt = static_cast<uint32_t>(millis());
    const bool contentReady = !showCatalogLoading && !catalogOpenPending &&
                              (!allTab() || (LIBRARY_CATALOG.isReady() && pinnedProjectionCurrent()));
    LOG_DBG("LIBT", "render_visible tab=%s ready=%u loading=%u books=%u view=%u draw_ms=%u display_ms=%u total_ms=%u",
            libraryTabName(allTab()), static_cast<unsigned>(contentReady), static_cast<unsigned>(showCatalogLoading),
            static_cast<unsigned>(bookCount), static_cast<unsigned>(viewMode()),
            static_cast<unsigned>(displayStartedAt - renderStartedAt),
            static_cast<unsigned>(visibleAt - displayStartedAt), static_cast<unsigned>(visibleAt - renderStartedAt));
    if (navigationFirstId != 0) {
      LOG_DBG("LIBT",
              "nav_visible ids=%u-%u input_to_render_ms=%u input_to_visible_ms=%u page_cache=%u snapshot=%d "
              "cover_batch=%u draw_ms=%u display_ms=%u",
              static_cast<unsigned>(navigationFirstId), static_cast<unsigned>(navigationLastId),
              static_cast<unsigned>(renderStartedAt - navigationInputAt),
              static_cast<unsigned>(visibleAt - navigationInputAt), renderPageCacheHit ? 1U : 0U, gridSnapshotHit,
              coverBatchRendered ? 1U : 0U, static_cast<unsigned>(displayStartedAt - renderStartedAt),
              static_cast<unsigned>(visibleAt - displayStartedAt));
      libraryTraceVisibleFirstId = 0;
      libraryTraceVisibleLastId = 0;
      libraryTraceVisibleInputAt = 0;
    }
    if (libraryTraceFirstVisiblePending && contentReady) {
      LOG_DBG("LIBT", "first_visible tab=%s books=%u tab_ms=%u activity_ms=%u", libraryTabName(allTab()),
              static_cast<unsigned>(bookCount), static_cast<unsigned>(visibleAt - libraryTraceTabStartedAt),
              static_cast<unsigned>(visibleAt - libraryTraceActivityStartedAt));
      libraryTraceFirstVisiblePending = false;
    }
#endif
  }
}
