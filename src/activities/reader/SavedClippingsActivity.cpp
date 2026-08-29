#include "SavedClippingsActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <utility>

#include "BookSavedItemsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"

namespace {

// These compact markers intentionally use the same 1-bit icon treatment as
// the rest of CrossVi.  They replace translated count labels in the narrow
// trailing lane of the Saved list, which otherwise leaves too little width for
// a book title and author on X3.
constexpr int SAVED_COUNT_ICON_SIZE = 16;
constexpr int SAVED_COUNT_NUMBER_ICON_GAP = 3;
constexpr int SAVED_COUNT_GROUP_GAP = 8;
constexpr int SAVED_LIST_VALUE_RIGHT_PADDING = 8;  // Matches CrossVi drawList().
constexpr int SAVED_LIST_TITLE_VALUE_GAP = 4;

// 16x16 paperclip-style marker for a saved text highlight (MSB first,
// 0 = ink).  Keep this local: it is a list-summary marker, not a general
// navigation icon.
constexpr uint8_t HighlightStatusIcon[] = {
    0xFF, 0xFF, 0xFC, 0x7F, 0xF8, 0x3F, 0xF3, 0x1F, 0xE7, 0x8F, 0xE7, 0xCF, 0xE7, 0xCF, 0xE7, 0xCF,
    0xE7, 0xCF, 0xE7, 0xCF, 0xE7, 0xCF, 0xE7, 0xCF, 0xE7, 0xCF, 0xF3, 0xCF, 0xF8, 0x1F, 0xFC, 0x7F,
};
static_assert(sizeof(HighlightStatusIcon) == 32);

std::string savedCountText(const uint16_t count) { return std::to_string(static_cast<unsigned>(count)); }

int savedCountSummaryWidth(const GfxRenderer& renderer, const SavedClippingsModel::SavedBookEntry& entry) {
  int width = 0;
  const auto addCount = [&](const uint16_t count) {
    if (count == 0) return;
    if (width != 0) width += SAVED_COUNT_GROUP_GAP;
    width += renderer.getTextWidth(UI_10_FONT_ID, savedCountText(count).c_str()) + SAVED_COUNT_NUMBER_ICON_GAP +
             SAVED_COUNT_ICON_SIZE;
  };
  addCount(entry.bookmarkCount);
  addCount(entry.highlightCount);
  return width;
}

void drawSavedCountSummary(const GfxRenderer& renderer, int x, const int rowY,
                           const SavedClippingsModel::SavedBookEntry& entry) {
  int cursorX = x;
  const auto drawCount = [&](const uint16_t count, const uint8_t* icon) {
    if (count == 0) return;
    const std::string text = savedCountText(count);
    renderer.drawText(UI_10_FONT_ID, cursorX, rowY + 16, text.c_str());
    cursorX += renderer.getTextWidth(UI_10_FONT_ID, text.c_str()) + SAVED_COUNT_NUMBER_ICON_GAP;
    renderer.drawIcon(icon, cursorX,
                      rowY + (CrossViMetrics::values.listWithSubtitleRowHeight - SAVED_COUNT_ICON_SIZE) / 2,
                      SAVED_COUNT_ICON_SIZE);
    cursorX += SAVED_COUNT_ICON_SIZE + SAVED_COUNT_GROUP_GAP;
  };

  drawCount(entry.bookmarkCount, BookmarkStatusIcon);
  drawCount(entry.highlightCount, HighlightStatusIcon);
}

}  // namespace

void SavedClippingsActivity::onEnter() {
  Activity::onEnter();
  if (pendingReturnState.has_value()) {
    selectedIndex_ = static_cast<int>(pendingReturnState->selectedIndex);
    restoredBookPath_ = std::move(pendingReturnState->selectedPath);
    pendingReturnState.reset();
  }
  RenderLock lock(*this);
  reloadCatalog(true);
  requestUpdate();
}

void SavedClippingsActivity::onExit() {
  openedStore_.unload();
  clippingCatalog_ = {};
  bookmarkCatalog_ = {};
  savedCatalog_ = {};
  restoredBookPath_.clear();
  Activity::onExit();
}

void SavedClippingsActivity::onResume() {
  // The child can delete the last clipping for a book or migrate a legacy
  // store. Re-scan only here, while returning to this screen; Home never pays
  // this SD I/O cost.
  RenderLock lock(*this);
  openedStore_.unload();
  reloadCatalog(false);
}

void SavedClippingsActivity::reloadCatalog(const bool clearNotice) {
  std::string selectedBookPath;
  if (!restoredBookPath_.empty()) selectedBookPath = restoredBookPath_;
  if (selectedIndex_ > 0 && selectedIndex_ <= static_cast<int>(savedCatalog_.entries.size())) {
    if (selectedBookPath.empty())
      selectedBookPath = savedCatalog_.entries[static_cast<size_t>(selectedIndex_ - 1)].path;
  }

  if (clearNotice) notice_.clear();
  clippingCatalogLoadResult_ = ClippingStore::loadCatalog(clippingCatalog_);
  SavedClippingsModel::sortEntries(clippingCatalog_.entries);
  bookmarkCatalogLoadResult_ = BookmarkCatalog::load(
      bookmarkCatalog_,
      [](const std::string& path, std::vector<BookmarkEntry>& bookmarks, BookmarkBookMetadata& metadata) {
        const auto status = JsonSettingsIO::loadBookmarksFromFile(bookmarks, path.c_str(), &metadata);
        switch (status) {
          case JsonSettingsIO::BookmarkLoadStatus::Loaded:
            return BookmarkCatalog::DocumentLoadResult::Loaded;
          case JsonSettingsIO::BookmarkLoadStatus::Missing:
            return BookmarkCatalog::DocumentLoadResult::Missing;
          case JsonSettingsIO::BookmarkLoadStatus::Invalid:
          case JsonSettingsIO::BookmarkLoadStatus::Oversize:
            return BookmarkCatalog::DocumentLoadResult::Invalid;
          case JsonSettingsIO::BookmarkLoadStatus::IoError:
            return BookmarkCatalog::DocumentLoadResult::IoError;
        }
        return BookmarkCatalog::DocumentLoadResult::Invalid;
      });
  savedCatalog_ = SavedClippingsModel::combine(clippingCatalogLoadResult_, clippingCatalog_, bookmarkCatalogLoadResult_,
                                               bookmarkCatalog_);

  if (!selectedBookPath.empty()) {
    const auto selected = std::find_if(savedCatalog_.entries.begin(), savedCatalog_.entries.end(),
                                       [&](const auto& entry) { return entry.path == selectedBookPath; });
    if (selected != savedCatalog_.entries.end()) {
      selectedIndex_ = static_cast<int>(std::distance(savedCatalog_.entries.begin(), selected)) + 1;
      restoredBookPath_.clear();
      return;
    }
  }
  selectedIndex_ = std::clamp(selectedIndex_, 0, rowCount() - 1);
  restoredBookPath_.clear();
}

void SavedClippingsActivity::handleClippingResult(const ClippingJumpResult& jump) {
  RenderLock lock(*this);
  if (jump.bookPath != openedStore_.book().path || jump.bookType != openedStore_.book().bookType ||
      jump.storePath != openedStore_.path()) {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    return;
  }
  HalFile book = Storage.open(jump.bookPath.c_str());
  const bool regularBook = book && !book.isDirectory();
  const bool bookClosed = !book || book.close();
  if (!regularBook || !bookClosed) {
    // The clipping remains viewable/exportable, but there is no book to open.
    // Never fall through to ReaderActivity, which would otherwise leave this
    // screen without being able to validate a target.
    notice_ = tr(STR_BOOK_FILE_MISSING);
    return;
  }
  const bool epubBook = jump.bookType == "epub" && FsHelpers::hasEpubExtension(jump.bookPath);
  const bool textBook = jump.bookType == "txt" &&
                        (FsHelpers::hasTxtExtension(jump.bookPath) || FsHelpers::hasMarkdownExtension(jump.bookPath));
  if (!epubBook && !textBook) {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    return;
  }

  const std::string bookPath = jump.bookPath;
  ClippingJumpResult request = jump;
  activityManager.captureSavedClippingsReturnContext(static_cast<size_t>(std::max(selectedIndex_, 0)), jump.bookPath);
  lock.unlock();
  activityManager.goToReader(bookPath, std::move(request), ReaderOpenOrigin::SavedItems);
}

void SavedClippingsActivity::handleSavedBookResult(const ActivityResult& result) {
  if (result.isCancelled) return;
  if (const auto* clipping = std::get_if<ClippingJumpResult>(&result.data)) {
    handleClippingResult(*clipping);
    return;
  }

  SavedBookmarkJumpResult jump;
  jump.bookPath = openedBookPath_;
  jump.bookType = openedBookType_;
  if (const auto* progress = std::get_if<ProgressChangeResult>(&result.data)) {
    jump.progress = *progress;
    jump.bookmarkFingerprint = progress->bookmarkFingerprint;
    jump.hasBookmarkFingerprint = progress->hasBookmarkFingerprint;
  } else if (const auto* page = std::get_if<PageResult>(&result.data)) {
    jump.page = page->page;
    jump.hasFixedPage = true;
    jump.bookmarkFingerprint = page->bookmarkFingerprint;
    jump.hasBookmarkFingerprint = page->hasBookmarkFingerprint;
  } else {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    return;
  }
  activityManager.captureSavedClippingsReturnContext(static_cast<size_t>(std::max(selectedIndex_, 0)), jump.bookPath);
  activityManager.goToReader(jump.bookPath, std::move(jump), ReaderOpenOrigin::SavedItems);
}

void SavedClippingsActivity::openSelectedBook() {
  if (selectedIndex_ <= 0 || selectedIndex_ > static_cast<int>(savedCatalog_.entries.size())) return;
  const auto& selected = savedCatalog_.entries[static_cast<size_t>(selectedIndex_ - 1)];
  if (!selected.bookExists) {
    notice_ = tr(STR_BOOK_FILE_MISSING);
    requestUpdate();
    return;
  }
  const bool epubBook = selected.bookType == "epub" && FsHelpers::hasEpubExtension(selected.path);
  const bool textBook = selected.bookType == "txt" &&
                        (FsHelpers::hasTxtExtension(selected.path) || FsHelpers::hasMarkdownExtension(selected.path));
  const bool fixedBook = selected.bookType == "xtc" && FsHelpers::hasXtcExtension(selected.path);
  if (!epubBook && !textBook && !fixedBook) {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    requestUpdate();
    return;
  }

  openedStore_.unload();
  ClippingStore* clippingStore = nullptr;
  if (selected.highlightCount != 0) {
    const ClippingStore::LoadResult load =
        openedStore_.loadForBook(selected.path, selected.title, selected.author, selected.bookType);
    if (!openedStore_.isLoaded() || openedStore_.size() == 0 || load == ClippingStore::LoadResult::Ready) {
      openedStore_.unload();
      notice_ = tr(STR_OPEN_SAVED_ITEMS_FAILED);
      requestUpdate();
      return;
    }
    clippingStore = &openedStore_;
  }

  BookSavedItemsActivity::ReaderKind readerKind = BookSavedItemsActivity::ReaderKind::Epub;
  if (selected.bookType == "txt") {
    readerKind = BookSavedItemsActivity::ReaderKind::Text;
  } else if (selected.bookType == "xtc") {
    readerKind = BookSavedItemsActivity::ReaderKind::FixedLayout;
  }
  openedBookPath_ = selected.path;
  openedBookType_ = selected.bookType;
  startActivityForResult(std::make_unique<BookSavedItemsActivity>(renderer, mappedInput, selected.path, selected.title,
                                                                  selected.author, readerKind, clippingStore),
                         [this](const ActivityResult& result) { handleSavedBookResult(result); });
}

void SavedClippingsActivity::exportAll() {
  {
    RenderLock lock(*this);
    if (!exportAvailable()) {
      notice_.clear();
      switch (SavedClippingsModel::state(clippingCatalogLoadResult_, clippingCatalog_)) {
        case SavedClippingsModel::CatalogState::Empty:
          notice_ = tr(STR_NO_HIGHLIGHTS_TO_EXPORT);
          break;
        case SavedClippingsModel::CatalogState::Incomplete:
          notice_ = tr(STR_SAVED_ITEMS_INCOMPLETE);
          break;
        case SavedClippingsModel::CatalogState::ReadError:
          notice_ = tr(STR_SAVED_ITEMS_READ_FAILED);
          break;
        case SavedClippingsModel::CatalogState::Ready:
          break;
      }
      requestUpdate();
      return;
    }
    notice_ = tr(STR_EXPORTING_CLIPPINGS);
  }

  // Give e-ink users explicit feedback before the bounded SD read begins.
  requestUpdateAndWait();

  std::string outputPath;
  const ClippingStore::ExportResult result = ClippingStore::exportCatalog(clippingCatalog_, outputPath);

  RenderLock lock(*this);
  switch (result) {
    case ClippingStore::ExportResult::Exported: {
      char message[128]{};
      std::snprintf(message, sizeof(message), tr(STR_EXPORT_SAVED_TO_FORMAT), outputPath.c_str());
      notice_ = message;
      break;
    }
    case ClippingStore::ExportResult::Empty:
      notice_ = tr(STR_NO_HIGHLIGHTS_TO_EXPORT);
      break;
    case ClippingStore::ExportResult::CatalogIncomplete:
      notice_ = tr(STR_SAVED_ITEMS_INCOMPLETE);
      break;
    case ClippingStore::ExportResult::SourceChanged:
      notice_ = tr(STR_SAVED_ITEMS_CHANGED);
      reloadCatalog(false);
      break;
    case ClippingStore::ExportResult::NoAvailableName:
    case ClippingStore::ExportResult::IoError:
      notice_ = tr(STR_EXPORT_CLIPPINGS_FAILED);
      break;
  }
  requestUpdate();
}

void SavedClippingsActivity::loop() {
  RenderLock lock(*this);

  navigator_.onNext([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, rowCount());
    notice_.clear();
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, rowCount());
    notice_.clear();
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lock.unlock();
    onGoHome(HomeMenuItem::SAVED_ITEMS);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == 0) {
      lock.unlock();
      exportAll();
    } else {
      openSelectedBook();
    }
  }
}

std::string SavedClippingsActivity::rowTitle(const int index) const {
  if (index == 0) return tr(STR_EXPORT_ALL_CLIPPINGS);
  if (index < 0 || index > static_cast<int>(savedCatalog_.entries.size())) return {};
  const auto& book = savedCatalog_.entries[static_cast<size_t>(index - 1)];
  return book.title.empty() ? std::string(tr(STR_UNNAMED)) : book.title;
}

std::string SavedClippingsActivity::rowSubtitle(const int index) const {
  if (index == 0) return tr(STR_EXPORT_CLIPPINGS_DESC);
  if (index < 0 || index > static_cast<int>(savedCatalog_.entries.size())) return {};
  const auto& entry = savedCatalog_.entries[static_cast<size_t>(index - 1)];
  std::string subtitle = entry.author;
  if (!entry.bookExists) {
    if (!subtitle.empty()) subtitle += " - ";
    subtitle += tr(STR_BOOK_FILE_MISSING);
  }
  return subtitle;
}

UIIcon SavedClippingsActivity::rowIcon(const int index) const { return index == 0 ? Transfer : Bookmark; }

std::string SavedClippingsActivity::statusText() const {
  if (!notice_.empty()) return notice_;
  if (savedCatalog_.readError) return tr(STR_SAVED_ITEMS_READ_FAILED);
  if (savedCatalog_.entries.empty()) return tr(STR_SAVED_ITEMS_EMPTY);
  return savedCatalog_.incomplete ? tr(STR_SAVED_ITEMS_INCOMPLETE) : tr(STR_SAVED_ITEMS_HELP);
}

void SavedClippingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = screen.y + metrics.topPadding;
  const int contentTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int helpHeight = metrics.listRowHeight;
  const int contentBottom = screen.y + screen.height - helpHeight - metrics.verticalSpacing;
  const Rect listRect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)};

  GUI.drawHeader(renderer, Rect{screen.x, headerTop, screen.width, metrics.headerHeight}, tr(STR_SAVED_ITEMS));
  GUI.drawList(
      renderer, listRect, rowCount(), selectedIndex_, [this](const int index) { return rowTitle(index); },
      [this](const int index) { return rowSubtitle(index); }, [this](const int index) { return rowIcon(index); },
      nullptr, false,
      [this](const int index) {
        if (index == 0) return !exportAvailable();
        return index > 0 && index <= static_cast<int>(savedCatalog_.entries.size()) &&
               !savedCatalog_.entries[static_cast<size_t>(index - 1)].bookExists;
      },
      nullptr, -1,
      [this](const int index) {
        if (index <= 0 || index > static_cast<int>(savedCatalog_.entries.size())) return 0;
        const auto& entry = savedCatalog_.entries[static_cast<size_t>(index - 1)];
        const int width = savedCountSummaryWidth(renderer, entry);
        return width == 0 ? 0 : width + SAVED_LIST_TITLE_VALUE_GAP;
      });

  const int rowHeight = CrossViMetrics::values.listWithSubtitleRowHeight;
  const int pageItems = listRect.height / rowHeight;
  if (pageItems > 0 && !savedCatalog_.entries.empty()) {
    const int totalPages = (rowCount() + pageItems - 1) / pageItems;
    const int contentWidth =
        listRect.width - (totalPages > 1 ? metrics.scrollBarWidth + metrics.scrollBarRightOffset : 1);
    const int pageStart = selectedIndex_ / pageItems * pageItems;
    const int rightEdge = listRect.x + contentWidth - metrics.contentSidePadding - SAVED_LIST_VALUE_RIGHT_PADDING;
    const int pageEnd = std::min(rowCount(), pageStart + pageItems);
    for (int index = std::max(1, pageStart); index < pageEnd; ++index) {
      const auto& entry = savedCatalog_.entries[static_cast<size_t>(index - 1)];
      const int width = savedCountSummaryWidth(renderer, entry);
      drawSavedCountSummary(renderer, rightEdge - width, listRect.y + (index % pageItems) * rowHeight, entry);
    }
  }

  const std::string status = statusText();
  GUI.drawHelpText(renderer, Rect{screen.x, contentBottom + metrics.verticalSpacing, screen.width, helpHeight},
                   status.c_str());

  const char* confirm = selectedIndex_ == 0 ? tr(STR_EXPORT) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
