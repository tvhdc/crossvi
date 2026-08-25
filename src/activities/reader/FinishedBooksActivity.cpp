#include "FinishedBooksActivity.h"

#include <ClockDateFormat.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include "BookReadingStats.h"
#include "BookStatsLoader.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsPresentation.h"
#include "ReadingStatsUtils.h"
#include "RecentBooksStore.h"
#include "components/LibraryGridModel.h"
#include "components/UITheme.h"

namespace {
std::string titleFor(const LibraryBookRecord& book) {
  if (!book.title.empty()) return book.title;
  const size_t slash = book.path.find_last_of('/');
  return slash == std::string::npos ? book.path : book.path.substr(slash + 1);
}

std::string subtitleFor(const LibraryBookRecord& book, const uint32_t finishedDay) {
  std::string value;
  ReadingStatsDate date;
  if (finishedDay != 0 && readingStatsDateFromDayIndex(finishedDay, date)) {
    char formatted[32]{};
    const char separator = ClockDateFormat::separatorChar(SETTINGS.dateSeparator);
    if (ClockDateFormat::format(date.year, date.month, date.day, SETTINGS.dateFormat, separator, formatted,
                                sizeof(formatted), I18N.getLanguage() == Language::VI)) {
      value = formatted;
    }
  }
  if (!book.author.empty()) {
    if (!value.empty()) value += " · ";
    value += book.author;
  }
  return value;
}
}  // namespace

void FinishedBooksActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  books_.clear();
  visibleBooks_.clear();
  visiblePageStart_ = 0;
  scanIndex_ = 0;
  catalogGeneration_ = 0;
  selected_ = 0;
  catalogScanStarted_ = false;
  catalogScanComplete_ = false;
  catalogScanPartial_ = false;
  statsLoadFailed_ = false;
  if (!LIBRARY_CATALOG.open() && !LIBRARY_CATALOG.isBuilding() && !LIBRARY_CATALOG.isReady()) {
    catalogScanComplete_ = true;
    catalogScanPartial_ = true;
  }
  requestUpdate();
}

void FinishedBooksActivity::onExit() {
  if (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding()) LIBRARY_CATALOG.cancel();
  books_.clear();
  visibleBooks_.clear();
  Activity::onExit();
}

void FinishedBooksActivity::stepCatalog() {
  if (catalogScanComplete_) return;
  if (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding()) {
    LIBRARY_CATALOG.step();
    return;
  }
  if (!LIBRARY_CATALOG.isReady()) {
    catalogScanComplete_ = true;
    catalogScanPartial_ = true;
    requestUpdate();
    return;
  }
  if (!catalogScanStarted_) {
    catalogScanStarted_ = true;
    catalogGeneration_ = LIBRARY_CATALOG.generation();
    scanIndex_ = 0;
    books_.clear();
    books_.reserve(std::min<uint32_t>(LIBRARY_CATALOG.count(), LibraryCatalogStore::MAX_BOOKS));
  }
  if (catalogGeneration_ != LIBRARY_CATALOG.generation()) {
    catalogScanStarted_ = false;
    return;
  }
  if (scanIndex_ >= LIBRARY_CATALOG.count()) {
    finishCatalogScan();
    return;
  }

  const uint32_t catalogIndex = scanIndex_++;
  LibraryBookRecord record;
  if (!LIBRARY_CATALOG.loadRecord(catalogIndex, record)) {
    catalogScanPartial_ = true;
    return;
  }
  BookReadingStats stats;
  const RecentBook recent{record.path, record.title, record.author, record.coverBmpPath};
  if (!loadTrustedBookReadingStats(recent, stats)) {
    catalogScanPartial_ = true;
    return;
  }
  if (!stats.isCompleted) return;

  uint16_t finishedDay = 0;
  if (stats.finishedDate.isValid()) {
    const uint32_t day = readingStatsDayIndex(stats.finishedDate);
    if (day <= UINT16_MAX) finishedDay = static_cast<uint16_t>(day);
  }
  books_.push_back({static_cast<uint16_t>(catalogIndex), finishedDay});
}

void FinishedBooksActivity::finishCatalogScan() {
  std::sort(books_.begin(), books_.end(), [](const CompletedBook& left, const CompletedBook& right) {
    if (left.finishedDay != right.finishedDay) return left.finishedDay > right.finishedDay;
    return left.catalogIndex < right.catalogIndex;
  });
  catalogScanComplete_ = true;
  selected_ = books_.empty() ? 0 : std::min(selected_, books_.size() - 1);
  if (!loadVisiblePage()) catalogScanPartial_ = true;
  requestUpdate();
}

bool FinishedBooksActivity::loadVisiblePage() {
  if (books_.empty()) {
    visibleBooks_.clear();
    visiblePageStart_ = 0;
    return true;
  }
  const int capacity = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true));
  const size_t pageStart = selected_ / static_cast<size_t>(capacity) * static_cast<size_t>(capacity);
  if (!visibleBooks_.empty() && pageStart == visiblePageStart_) return true;
  visibleBooks_.clear();
  visiblePageStart_ = pageStart;
  const size_t count = std::min(static_cast<size_t>(capacity), books_.size() - visiblePageStart_);
  std::vector<size_t> indices;
  indices.reserve(count);
  for (size_t offset = 0; offset < count; ++offset) {
    indices.push_back(books_[visiblePageStart_ + offset].catalogIndex);
  }
  return LIBRARY_CATALOG.loadRecords(indices, visibleBooks_) && visibleBooks_.size() == count;
}

const LibraryBookRecord* FinishedBooksActivity::visibleRecord(const size_t index) const {
  if (index < visiblePageStart_ || index - visiblePageStart_ >= visibleBooks_.size()) return nullptr;
  return &visibleBooks_[index - visiblePageStart_];
}

void FinishedBooksActivity::openSelectedStatistics() {
  if (!catalogScanComplete_ || selected_ >= books_.size()) return;
  const LibraryBookRecord* selected = visibleRecord(selected_);
  if (!selected) {
    statsLoadFailed_ = true;
    requestUpdate();
    return;
  }
  const RecentBook recent{selected->path, selected->title, selected->author, selected->coverBmpPath};
  ReadingStatsPresentation presentation;
  if (!loadBookStatsPresentation(recent, presentation)) {
    statsLoadFailed_ = true;
    requestUpdate();
    return;
  }
  statsLoadFailed_ = false;
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, titleFor(*selected), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, false, false, selected->path),
      [this](const ActivityResult&) { requestUpdate(); });
}

void FinishedBooksActivity::move(const int delta) {
  const size_t count = books_.size();
  if (count == 0) return;
  const size_t previous = selected_;
  selected_ =
      delta < 0 ? LibraryGridModel::previousIndex(selected_, count) : LibraryGridModel::nextIndex(selected_, count);
  if (selected_ != previous && !loadVisiblePage()) catalogScanPartial_ = true;
  statsLoadFailed_ = false;
  requestUpdate();
}

void FinishedBooksActivity::loop() {
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
  if (!catalogScanComplete_) {
    stepCatalog();
    return;
  }
  navigator_.onPrevious([this] { move(-1); });
  navigator_.onNext([this] { move(1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelectedStatistics();
}

void FinishedBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_FINISHED_BOOKS));
  const int contentTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = safe.y + safe.height - metrics.verticalSpacing - contentTop;
  const bool empty = books_.empty();
  if (statsLoadFailed_) {
    GUI.drawList(renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentHeight)}, 1, -1,
                 [](int) { return std::string(tr(STR_STATS_UNAVAILABLE)); });
  } else if (!catalogScanComplete_) {
    GUI.drawList(renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentHeight)}, 1, -1,
                 [](int) { return std::string(tr(STR_STATS_LOADING_LIBRARY)); });
  } else {
    GUI.drawList(
        renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentHeight)}, empty ? 1 : books_.size(),
        empty ? -1 : static_cast<int>(selected_),
        [this, empty](const int index) {
          const LibraryBookRecord* book = visibleRecord(static_cast<size_t>(index));
          return empty  ? std::string(I18N.get(catalogScanPartial_ ? StrId::STR_FINISHED_BOOKS_PARTIAL
                                                                   : StrId::STR_STATS_NO_DATA))
                 : book ? titleFor(*book)
                        : std::string(tr(STR_STATS_UNAVAILABLE));
        },
        [this, empty](const int index) {
          const LibraryBookRecord* book = visibleRecord(static_cast<size_t>(index));
          return empty || !book ? std::string{} : subtitleFor(*book, books_[static_cast<size_t>(index)].finishedDay);
        },
        [this, empty](const int index) {
          const LibraryBookRecord* book = visibleRecord(static_cast<size_t>(index));
          return empty || !book ? UIIcon::None : UITheme::getFileIcon(book->path);
        });
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
