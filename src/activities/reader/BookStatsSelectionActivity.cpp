#include "BookStatsSelectionActivity.h"

#include <Epub.h>
#include <Epub/SourceIdentityStore.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

#include "BookReadingStats.h"
#include "BookStatsLoader.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsPresentation.h"
#include "components/UITheme.h"

namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";

const char* formatLabel(const std::string& path) {
  if (FsHelpers::hasTxtExtension(path)) return "TXT";
  if (FsHelpers::hasMarkdownExtension(path)) return "MD";
  if (FsHelpers::checkFileExtension(path, ".xtch")) return "XTCH";
  if (FsHelpers::hasXtcExtension(path)) return "XTC";
  return "EPUB";
}

std::string displayTitle(const RecentBook& book) {
  if (!book.title.empty()) return book.title;
  const size_t separator = book.path.find_last_of('/');
  return separator == std::string::npos ? book.path : book.path.substr(separator + 1);
}

bool trustedIdentity(const std::string& cachePath, const ZipFile::SourceIdentity& current) {
  ZipFile::SourceIdentity stored;
  const SourceIdentityStore::LoadStatus status = SourceIdentityStore::load(cachePath, stored);
  return (status == SourceIdentityStore::LoadStatus::Primary || status == SourceIdentityStore::LoadStatus::Backup ||
          status == SourceIdentityStore::LoadStatus::Temp) &&
         stored == current;
}

bool hasBookStatsArtifact(const std::string& cachePath) {
  constexpr std::array<const char*, 10> names = {
      "stats_v6.bin",     "stats_v6.bin.bak", "stats_v6.bin.tmp", "stats_v5.bin",  "stats_v5.bin.bak",
      "stats_v5.bin.tmp", "stats_v4.bin",     "stats.bin",        "stats.bin.bak", "stats.bin.tmp"};
  return std::any_of(names.begin(), names.end(),
                     [&cachePath](const char* name) { return Storage.exists((cachePath + "/" + name).c_str()); });
}

bool resolveBookStatsCache(const RecentBook& recent, std::string& cachePath, bool& plainText) {
  plainText = false;
  if (FsHelpers::hasEpubExtension(recent.path)) {
    Epub book(recent.path, CACHE_ROOT);
    cachePath = book.getCachePath();
    return !hasBookStatsArtifact(cachePath) || book.inspectSourceBinding() == Epub::SourceBindingStatus::Match;
  }
  if (FsHelpers::hasTxtExtension(recent.path) || FsHelpers::hasMarkdownExtension(recent.path)) {
    Txt book(recent.path, CACHE_ROOT);
    cachePath = book.getCachePath();
    ZipFile::SourceIdentity identity;
    if (hasBookStatsArtifact(cachePath) &&
        (!book.load() || !book.getSourceIdentity(identity) || !trustedIdentity(cachePath, identity))) {
      return false;
    }
    plainText = true;
    return true;
  }
  if (FsHelpers::hasXtcExtension(recent.path)) {
    Xtc book(recent.path, CACHE_ROOT);
    cachePath = book.getCachePath();
    ZipFile::SourceIdentity identity;
    return !hasBookStatsArtifact(cachePath) ||
           (book.load() && book.getSourceIdentity(identity) && trustedIdentity(cachePath, identity));
  }
  return false;
}

}  // namespace

bool loadTrustedBookReadingStats(const RecentBook& recent, BookReadingStats& stats, bool* plainText) {
  std::string cachePath;
  bool isPlainText = false;
  if (!resolveBookStatsCache(recent, cachePath, isPlainText)) return false;
  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Missing;
  BookReadingStats loaded = BookReadingStats::load(cachePath, &bookStatus);
  if (!BookReadingStats::isTrustedLoadStatus(bookStatus)) return false;
  stats = std::move(loaded);
  if (plainText) *plainText = isPlainText;
  return true;
}

bool loadBookStatsPresentation(const RecentBook& recent, ReadingStatsPresentation& presentation) {
  BookReadingStats bookStats;
  bool plainText = false;
  if (!loadTrustedBookReadingStats(recent, bookStats, &plainText)) return false;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats globalStats = GlobalReadingStats::load(&globalStatus);
  if (!GlobalReadingStats::isTrustedLoadStatus(globalStatus)) {
    return false;
  }
  const GlobalReadingStatsAggregation aggregate = GlobalReadingStats::hasSyncedStats()
                                                      ? GlobalReadingStats::loadAggregatedWithReport(globalStats)
                                                      : GlobalReadingStatsAggregation{};
  ReadingStatsDateTime now;
  const ReadingStatsDateTime* current = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  presentation = buildReadingStatsPresentation(bookStats, true, globalStats, true, aggregate, current,
                                               ReadingStatsMetric::unavailable(), false);
  if (plainText) markReadingStatsPageMetricsNotApplicable(presentation);
  return true;
}

void BookStatsSelectionActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  pageSize_ = pageCapacity();
  loadRecentBooks();
  requestUpdate();
}

void BookStatsSelectionActivity::onExit() {
  recentBooks_.clear();
  Activity::onExit();
}

void BookStatsSelectionActivity::loadRecentBooks() {
  recentBooks_.clear();
  const auto& stored = RECENT_BOOKS.getBooks();
  recentBooks_.reserve(stored.size());

  // Match the Recent tab: pinned recent books first, then the remaining books
  // in recency order. This is an in-memory projection and never opens the
  // library catalog or per-book statistics.
  for (const std::string& path : RECENT_BOOKS.getPinnedPaths()) {
    if (SETTINGS.hideTxtBooks && FsHelpers::hasTxtExtension(path)) continue;
    const auto book =
        std::find_if(stored.begin(), stored.end(), [&path](const RecentBook& item) { return item.path == path; });
    if (book != stored.end()) recentBooks_.push_back(*book);
  }
  for (const RecentBook& book : stored) {
    if (SETTINGS.hideTxtBooks && FsHelpers::hasTxtExtension(book.path)) continue;
    if (!RECENT_BOOKS.isPinned(book.path)) recentBooks_.push_back(book);
  }

  const auto restored = std::find_if(recentBooks_.begin(), recentBooks_.end(), [this](const RecentBook& book) {
    return !selectedPath_.empty() && book.path == selectedPath_;
  });
  if (restored != recentBooks_.end()) {
    selectedIndex_ = static_cast<size_t>(std::distance(recentBooks_.begin(), restored));
  } else {
    selectedIndex_ = LibraryGridModel::clampIndex(selectedIndex_, recentBooks_.size());
  }
  updateRenderState();
  rememberSelectedPath();
}

void BookStatsSelectionActivity::updateRenderState() {
  RenderLock lock(*this);
  renderState_.visibleCount = recentBooks_.size();
  renderState_.selectedIndex = selectedIndex_;
  renderState_.pageSize = pageSize_;
  renderState_.statsLoadFailed = false;
}

void BookStatsSelectionActivity::rememberSelectedPath() {
  const RecentBook* record = selectedRecord();
  if (record) selectedPath_ = record->path;
}

size_t BookStatsSelectionActivity::visibleCount() const { return recentBooks_.size(); }

size_t BookStatsSelectionActivity::pageCapacity() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  return static_cast<size_t>(std::max(1, GUI.getListPageItems(std::max(1, contentBottom - contentTop), true)));
}

const RecentBook* BookStatsSelectionActivity::selectedRecord() const {
  return selectedIndex_ < recentBooks_.size() ? &recentBooks_[selectedIndex_] : nullptr;
}

void BookStatsSelectionActivity::openSelectedBook() {
  RecentBook record;
  {
    RenderLock lock(*this);
    const RecentBook* selected = selectedRecord();
    if (!selected) return;
    record = *selected;
    renderState_.statsLoadFailed = false;
  }
  ReadingStatsPresentation presentation;
  if (!loadBookStatsPresentation(record, presentation)) {
    {
      RenderLock lock(*this);
      renderState_.statsLoadFailed = true;
    }
    requestUpdate();
    return;
  }
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, displayTitle(record), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, false, false),
      [this](const ActivityResult&) { requestUpdate(); });
}

void BookStatsSelectionActivity::moveSelection(const int delta) {
  const size_t count = visibleCount();
  if (count == 0 || delta == 0) return;
  const size_t previous = selectedIndex_;
  selectedIndex_ = delta < 0 ? LibraryGridModel::previousIndex(selectedIndex_, count)
                             : LibraryGridModel::nextIndex(selectedIndex_, count);
  if (selectedIndex_ == previous) return;
  updateRenderState();
  rememberSelectedPath();
  requestUpdate();
}

void BookStatsSelectionActivity::movePage(const int delta) {
  const size_t count = visibleCount();
  if (count == 0 || delta == 0) return;
  const size_t pageCount = LibraryGridModel::pageCount(count, pageSize_);
  if (pageCount <= 1) return;
  const size_t currentPage = selectedIndex_ / pageSize_;
  const size_t slot = selectedIndex_ % pageSize_;
  const size_t targetPage =
      delta > 0 ? (currentPage + 1) % pageCount : (currentPage == 0 ? pageCount - 1 : currentPage - 1);
  selectedIndex_ = std::min(targetPage * pageSize_ + slot, count - 1);
  updateRenderState();
  rememberSelectedPath();
  requestUpdate();
}

void BookStatsSelectionActivity::loop() {
  if (suppressInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease_ = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const size_t capacity = pageCapacity();
  if (capacity != pageSize_) {
    pageSize_ = capacity;
    updateRenderState();
    requestUpdate();
  }

  navigator_.onNextRelease([this] { moveSelection(1); });
  navigator_.onPreviousRelease([this] { moveSelection(-1); });
  navigator_.onContinuous({MappedInputManager::Button::Right}, [this] { movePage(1); });
  navigator_.onContinuous({MappedInputManager::Button::Left}, [this] { movePage(-1); });
  navigator_.onContinuous({MappedInputManager::Button::Down}, [this] { moveSelection(1); });
  navigator_.onContinuous({MappedInputManager::Button::Up}, [this] { moveSelection(-1); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelectedBook();
}

void BookStatsSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  char pageLabel[16]{};
  if (renderState_.visibleCount > 0) {
    std::snprintf(pageLabel, sizeof(pageLabel), "%u/%u",
                  static_cast<unsigned>(LibraryGridModel::pageNumber(renderState_.selectedIndex,
                                                                     renderState_.visibleCount, renderState_.pageSize)),
                  static_cast<unsigned>(LibraryGridModel::pageCount(renderState_.visibleCount, renderState_.pageSize)));
  }
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_READING_STATS),
                 pageLabel[0] == '\0' ? nullptr : pageLabel);
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, tr(STR_STATS_SELECT_BOOK));
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  const int listHeight =
      std::min(contentBottom - contentTop, static_cast<int>(renderState_.pageSize) * metrics.listWithSubtitleRowHeight);
  const Rect content{safe.x, contentTop, safe.width, std::max(1, listHeight)};

  if (renderState_.statsLoadFailed) {
    GUI.drawList(renderer, content, 1, -1, [](int) { return std::string(tr(STR_STATS_UNAVAILABLE)); });
  } else {
    const bool empty = renderState_.visibleCount == 0;
    const int itemCount = empty ? 1 : static_cast<int>(renderState_.visibleCount);
    const int selectedRow = empty ? -1 : static_cast<int>(renderState_.selectedIndex);
    GUI.drawList(
        renderer, content, itemCount, selectedRow,
        [this, empty](const int index) {
          if (empty) return std::string(tr(STR_NO_RECENT_BOOKS));
          return index >= 0 && static_cast<size_t>(index) < recentBooks_.size()
                     ? displayTitle(recentBooks_[static_cast<size_t>(index)])
                     : std::string{};
        },
        [this, empty](const int index) {
          if (empty || index < 0 || static_cast<size_t>(index) >= recentBooks_.size()) return std::string{};
          const RecentBook& book = recentBooks_[static_cast<size_t>(index)];
          return book.author.empty() ? std::string(formatLabel(book.path)) : book.author;
        },
        [this, empty](const int index) {
          if (empty || index < 0 || static_cast<size_t>(index) >= recentBooks_.size()) return UIIcon::None;
          return UITheme::getFileIcon(recentBooks_[static_cast<size_t>(index)].path);
        },
        nullptr, false, nullptr, nullptr, empty ? -1 : static_cast<int>(renderState_.selectedIndex));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
