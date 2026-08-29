#include "BookSavedItemsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>

#include "BookSavedItemsNavigation.h"
#include "ClippingListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/BookmarkUtil.h"

namespace {

constexpr unsigned long DELETE_HOLD_MS = 700;

BookmarkEntry::PositionKind positionKind(const BookSavedItemsActivity::ReaderKind kind) {
  switch (kind) {
    case BookSavedItemsActivity::ReaderKind::Text:
      return BookmarkEntry::PositionKind::Text;
    case BookSavedItemsActivity::ReaderKind::FixedLayout:
      return BookmarkEntry::PositionKind::FixedLayout;
    case BookSavedItemsActivity::ReaderKind::Epub:
    default:
      return BookmarkEntry::PositionKind::Epub;
  }
}

}  // namespace

BookSavedItemsActivity::BookSavedItemsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::shared_ptr<Epub> epub, ClippingStore* clippingStore)
    : Activity("BookSavedItems", renderer, mappedInput),
      epub_(std::move(epub)),
      bookPath_(epub_ ? epub_->getPath() : std::string()),
      bookTitle_(epub_ ? epub_->getTitle() : std::string()),
      bookAuthor_(epub_ ? epub_->getAuthor() : std::string()),
      clippingStore_(clippingStore) {}

BookSavedItemsActivity::BookSavedItemsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::string bookPath, std::string bookTitle, std::string bookAuthor,
                                               const ReaderKind readerKind, ClippingStore* clippingStore,
                                               const uint32_t fixedPageCount)
    : Activity("BookSavedItems", renderer, mappedInput),
      bookPath_(std::move(bookPath)),
      bookTitle_(std::move(bookTitle)),
      bookAuthor_(std::move(bookAuthor)),
      readerKind_(readerKind),
      clippingStore_(clippingStore),
      fixedPageCount_(fixedPageCount) {}

void BookSavedItemsActivity::onEnter() {
  Activity::onEnter();
  if (!clippingStore_ && readerKind_ != ReaderKind::FixedLayout && !bookPath_.empty()) {
    ownedClippingStore_ = std::make_unique<ClippingStore>();
    const auto result = ownedClippingStore_->loadForBook(bookPath_, bookTitle_, bookAuthor_,
                                                         readerKind_ == ReaderKind::Text ? "txt" : "epub");
    if (result == ClippingStore::LoadResult::Ready || result == ClippingStore::LoadResult::Loaded ||
        result == ClippingStore::LoadResult::Recovered || result == ClippingStore::LoadResult::Migrated ||
        result == ClippingStore::LoadResult::LoadedLegacy) {
      clippingStore_ = ownedClippingStore_.get();
    } else {
      ownedClippingStore_.reset();
    }
  }
  loadBookmarks();
  loadHighlightPreviews();
  rebuildProjection();
  requestUpdate();
}

void BookSavedItemsActivity::loadHighlightPreviews() {
  highlightPreviews_.clear();
  if (clippingStore_) {
    highlightPreviews_.reserve(clippingStore_->size());
    for (size_t index = 0; index < clippingStore_->size(); ++index) {
      std::string preview;
      if (clippingStore_->readText(index, preview)) preview = BookmarkUtil::sanitizeBookmarkSummary(std::move(preview));
      highlightPreviews_.push_back(std::move(preview));
    }
  }
}

bool BookSavedItemsActivity::bookmarkKindMatches(const BookmarkEntry& bookmark) const {
  if (bookmark.positionKind != positionKind(readerKind_)) return false;
  return readerKind_ != ReaderKind::FixedLayout || fixedPageCount_ == 0 || bookmark.pageIndex < fixedPageCount_;
}

void BookSavedItemsActivity::loadBookmarks() {
  bookmarks_.clear();
  bookmarksWritable_ = false;
  if (bookPath_.empty()) {
    storageError_ = true;
    return;
  }

  const std::string canonicalPath = BookmarkUtil::getBookmarkPath(bookPath_);
  const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(bookPath_);
  const std::string path = BookmarkUtil::canonicalFamilyExists(bookPath_) ? canonicalPath : legacyPath;
  BookmarkBookMetadata metadata;
  const auto status = JsonSettingsIO::loadBookmarksFromFile(bookmarks_, path.c_str(), &metadata);
  if (status != JsonSettingsIO::BookmarkLoadStatus::Loaded && status != JsonSettingsIO::BookmarkLoadStatus::Missing) {
    bookmarks_.clear();
    storageError_ = true;
    return;
  }
  if (!BookmarkUtil::metadataMatchesBook(metadata, bookPath_, positionKind(readerKind_))) {
    bookmarks_.clear();
    storageError_ = true;
    return;
  }
  bookmarksWritable_ = std::all_of(bookmarks_.begin(), bookmarks_.end(),
                                   [this](const BookmarkEntry& bookmark) { return bookmarkKindMatches(bookmark); });
  if (!bookmarksWritable_) {
    bookmarks_.clear();
    storageError_ = true;
  } else if (!bookmarks_.empty() && metadata.path.empty() && !saveBookmarks()) {
    // Legacy bookmark JSON remains usable even if optional catalog metadata
    // cannot be added. Never hide or discard its entries.
    storageError_ = true;
  }
}

void BookSavedItemsActivity::rebuildProjection() {
  items_ = BookSavedItemsModel::project(bookmarks_, clippingStore_, tab_);
  restoreNavigation();
}

uint8_t BookSavedItemsActivity::tabCount() const { return readerKind_ == ReaderKind::FixedLayout ? 1 : 3; }

const char* BookSavedItemsActivity::currentTabLabel() const {
  if (readerKind_ == ReaderKind::FixedLayout) return tr(STR_BOOKMARKS);
  switch (tab_) {
    case BookSavedItemsModel::Tab::Bookmarks:
      return tr(STR_BOOKMARKS);
    case BookSavedItemsModel::Tab::Highlights:
      return tr(STR_HIGHLIGHTS);
    case BookSavedItemsModel::Tab::All:
    default:
      return tr(STR_ALL);
  }
}

void BookSavedItemsActivity::restoreNavigation() {
  BookSavedItemsNavigation::State state{tabFocused_, static_cast<uint16_t>(std::max(0, selectedIndex_))};
  state = BookSavedItemsNavigation::restore(state, static_cast<uint16_t>(items_.size()));
  tabFocused_ = state.tabFocused;
  selectedIndex_ = state.selectedItem;
}

bool BookSavedItemsActivity::moveSelectionNext() {
  BookSavedItemsNavigation::State state{tabFocused_, static_cast<uint16_t>(std::max(0, selectedIndex_))};
  const bool changed = BookSavedItemsNavigation::next(state, static_cast<uint16_t>(items_.size()));
  tabFocused_ = state.tabFocused;
  selectedIndex_ = state.selectedItem;
  return changed;
}

bool BookSavedItemsActivity::moveSelectionPrevious() {
  BookSavedItemsNavigation::State state{tabFocused_, static_cast<uint16_t>(std::max(0, selectedIndex_))};
  const bool changed = BookSavedItemsNavigation::previous(state, static_cast<uint16_t>(items_.size()));
  tabFocused_ = state.tabFocused;
  selectedIndex_ = state.selectedItem;
  return changed;
}

bool BookSavedItemsActivity::focusTabOrExit() {
  BookSavedItemsNavigation::State state{tabFocused_, static_cast<uint16_t>(std::max(0, selectedIndex_))};
  const auto action = BookSavedItemsNavigation::back(state, static_cast<uint16_t>(items_.size()));
  tabFocused_ = state.tabFocused;
  selectedIndex_ = state.selectedItem;
  return action == BookSavedItemsNavigation::BackAction::Exit;
}

void BookSavedItemsActivity::changeTab(const int direction) {
  const uint8_t count = tabCount();
  const uint8_t current = BookSavedItemsNavigation::clampTab(static_cast<uint8_t>(tab_), count);
  const uint8_t next = direction < 0 ? BookSavedItemsNavigation::previousTab(current, count)
                                     : BookSavedItemsNavigation::nextTab(current, count);
  tab_ = static_cast<BookSavedItemsModel::Tab>(next);
  selectedIndex_ = 0;
  tabFocused_ = true;
  rebuildProjection();
  requestUpdate();
}

void BookSavedItemsActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BookSavedItemsActivity::openSelected() {
  if (items_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
  const auto selected = items_[static_cast<size_t>(selectedIndex_)];
  if (selected.kind == BookSavedItemsModel::Kind::Highlight) {
    if (!clippingStore_ || selected.sourceIndex >= clippingStore_->size()) return;
    startActivityForResult(
        std::make_unique<ClippingListActivity>(renderer, mappedInput, *clippingStore_, selected.sourceIndex, true),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            ActivityResult forwarded = result;
            setResult(std::move(forwarded));
            finish();
          } else {
            loadHighlightPreviews();
            rebuildProjection();
          }
        });
    return;
  }

  if (selected.sourceIndex >= bookmarks_.size()) return;
  const BookmarkEntry& bookmark = bookmarks_[selected.sourceIndex];
  if (readerKind_ == ReaderKind::FixedLayout) {
    setResult(PageResult{bookmark.pageIndex, BookmarkUtil::fingerprint(bookmark), true});
    finish();
    return;
  }

  ProgressChangeResult result{};
  result.xpath = bookmark.xpath;
  result.percentage = bookmark.percentage;
  result.hasSavedProgress = true;
  result.spineIndex = bookmark.computedSpineIndex;
  result.contentSourceOffset = bookmark.contentSourceOffset;
  result.hasContentSourceOffset = bookmark.hasContentSourceOffset;
  result.bookmarkFingerprint = BookmarkUtil::fingerprint(bookmark);
  result.hasBookmarkFingerprint = true;
  if (readerKind_ == ReaderKind::Text) {
    result.textByteOffset = bookmark.byteOffset;
    result.hasTextByteOffset = true;
  } else if (epub_ && bookmark.computedChapterPageCount > 0 &&
             bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
             bookmark.computedSpineIndex < epub_->getSpineItemsCount()) {
    result.page = bookmark.computedChapterProgress;
    result.totalPages = bookmark.computedChapterPageCount;
  }
  setResult(std::move(result));
  finish();
}

bool BookSavedItemsActivity::saveBookmarks() {
  if (!bookmarksWritable_) return false;
  const std::string directory = BookmarkUtil::getBookmarksDir();
  if (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str())) return false;
  const BookmarkBookMetadata metadata{bookPath_, bookTitle_, bookAuthor_,
                                      BookmarkUtil::positionKindName(positionKind(readerKind_))};
  return JsonSettingsIO::saveBookmarks(bookmarks_, BookmarkUtil::getBookmarkPath(bookPath_).c_str(), &metadata);
}

void BookSavedItemsActivity::beginDelete() {
  if (items_.empty()) return;
  view_ = View::DeleteConfirm;
  longPressHandled_ = true;
  ignoreConfirmRelease_ = true;
  confirmPressSeen_ = false;
  requestUpdate();
}

void BookSavedItemsActivity::deleteSelected() {
  if (items_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
  const auto selected = items_[static_cast<size_t>(selectedIndex_)];
  bool removed = false;
  if (selected.kind == BookSavedItemsModel::Kind::Highlight) {
    removed =
        clippingStore_ && selected.sourceIndex < clippingStore_->size() && clippingStore_->remove(selected.sourceIndex);
    if (removed && selected.sourceIndex < highlightPreviews_.size()) {
      highlightPreviews_.erase(highlightPreviews_.begin() + selected.sourceIndex);
    }
  } else if (selected.sourceIndex < bookmarks_.size() && bookmarksWritable_) {
    const std::vector<BookmarkEntry> previous = bookmarks_;
    bookmarks_.erase(bookmarks_.begin() + selected.sourceIndex);
    removed = saveBookmarks();
    if (!removed) bookmarks_ = previous;
  }

  if (!removed) storageError_ = true;
  view_ = View::List;
  rebuildProjection();
  requestUpdate();
}

void BookSavedItemsActivity::loop() {
  RenderLock lock(*this);
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen_ = true;
    longPressHandled_ = false;
  }

  if (view_ == View::DeleteConfirm) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::List;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (ignoreConfirmRelease_) {
        ignoreConfirmRelease_ = false;
      } else {
        deleteSelected();
      }
      return;
    }
    return;
  }

  if (!tabFocused_ && confirmPressSeen_ && !longPressHandled_ &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Confirm) >= DELETE_HOLD_MS) {
    beginDelete();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmPressSeen_ = false;
    if (ignoreConfirmRelease_) {
      ignoreConfirmRelease_ = false;
      return;
    }
    if (tabFocused_) {
      if (tabCount() > 1) {
        changeTab(1);
      } else if (moveSelectionNext()) {
        requestUpdate();
      }
    } else {
      openSelected();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (focusTabOrExit()) {
      cancel();
    } else {
      requestUpdate();
    }
    return;
  }
  navigator_.onNextRelease([this] {
    if (moveSelectionNext()) requestUpdate();
  });
  navigator_.onPreviousRelease([this] {
    if (moveSelectionPrevious()) requestUpdate();
  });
  navigator_.onContinuous({MappedInputManager::Button::Right}, [this] {
    if (moveSelectionNext()) requestUpdate();
  });
  navigator_.onContinuous({MappedInputManager::Button::Left}, [this] {
    if (moveSelectionPrevious()) requestUpdate();
  });
  if (tabCount() > 1) {
    navigator_.onContinuous({MappedInputManager::Button::Down}, [this] { changeTab(1); });
    navigator_.onContinuous({MappedInputManager::Button::Up}, [this] { changeTab(-1); });
  }
}

std::string BookSavedItemsActivity::rowTitle(const int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) return {};
  const auto item = items_[static_cast<size_t>(index)];
  if (item.kind == BookSavedItemsModel::Kind::Bookmark) {
    if (item.sourceIndex >= bookmarks_.size()) return {};
    const std::string& summary = bookmarks_[item.sourceIndex].summary;
    return summary.empty() ? std::string(tr(STR_BOOKMARKS)) : summary;
  }
  if (item.sourceIndex < highlightPreviews_.size() && !highlightPreviews_[item.sourceIndex].empty()) {
    return highlightPreviews_[item.sourceIndex];
  }
  return tr(STR_HIGHLIGHTS);
}

std::string BookSavedItemsActivity::rowSubtitle(const int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) return {};
  const auto item = items_[static_cast<size_t>(index)];
  if (item.kind == BookSavedItemsModel::Kind::Bookmark) {
    if (item.sourceIndex >= bookmarks_.size()) return {};
    const BookmarkEntry& bookmark = bookmarks_[item.sourceIndex];
    if (bookmark.positionKind == BookmarkEntry::PositionKind::FixedLayout) {
      if (fixedPageCount_ == 0) {
        char page[32]{};
        std::snprintf(page, sizeof(page), tr(STR_PAGE_NUMBER_FORMAT), static_cast<unsigned>(bookmark.pageIndex + 1));
        return page;
      }
      const uint32_t count = std::max<uint32_t>(fixedPageCount_, bookmark.pageIndex + 1);
      return std::to_string(bookmark.pageIndex + 1) + "/" + std::to_string(count);
    }
    if (bookmark.positionKind == BookmarkEntry::PositionKind::Text) {
      return std::to_string(static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "%";
    }
    std::string subtitle = std::to_string(bookmark.computedChapterProgress + 1);
    if (bookmark.computedChapterPageCount > 0) subtitle += "/" + std::to_string(bookmark.computedChapterPageCount);
    subtitle +=
        " - " + std::to_string(static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "%";
    return subtitle;
  }
  if (!clippingStore_ || item.sourceIndex >= clippingStore_->size()) return {};
  const auto* clipping = clippingStore_->at(item.sourceIndex);
  if (!clipping) return {};
  return std::to_string(clipping->startPage + 1) + "/" + std::to_string(clipping->pageCount);
}

UIIcon BookSavedItemsActivity::rowIcon(const int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) return UIIcon::None;
  return items_[static_cast<size_t>(index)].kind == BookSavedItemsModel::Kind::Bookmark ? UIIcon::Bookmark
                                                                                        : UIIcon::Text;
}

void BookSavedItemsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 bookTitle_.empty() ? tr(STR_BOOKMARKS_AND_HIGHLIGHTS) : bookTitle_.c_str());

  const std::array<TabInfo, 3> tabItems = {
      {{tr(STR_ALL), tab_ == BookSavedItemsModel::Tab::All},
       {tr(STR_BOOKMARKS), readerKind_ == ReaderKind::FixedLayout || tab_ == BookSavedItemsModel::Tab::Bookmarks},
       {tr(STR_HIGHLIGHTS), tab_ == BookSavedItemsModel::Tab::Highlights}}};
  const std::span<const TabInfo> visibleTabs = readerKind_ == ReaderKind::FixedLayout
                                                   ? std::span<const TabInfo>(&tabItems[1], 1)
                                                   : std::span<const TabInfo>(tabItems);
  const int tabY = screen.y + metrics.topPadding + metrics.headerHeight;
  GUI.drawTabBar(renderer, Rect{screen.x, tabY, screen.width, metrics.tabBarHeight}, visibleTabs, tabFocused_);
  const int contentTop = tabY + metrics.tabBarHeight + metrics.verticalSpacing;
  const int helpHeight = metrics.listRowHeight;
  const int contentBottom = screen.y + screen.height - helpHeight - metrics.verticalSpacing;

  if (view_ == View::DeleteConfirm && !items_.empty()) {
    GUI.drawHelpText(renderer, Rect{screen.x, contentTop, screen.width, helpHeight}, tr(STR_CONFIRM_DELETE_SAVED_ITEM));
    GUI.drawList(
        renderer, Rect{screen.x, contentTop + helpHeight, screen.width, metrics.listWithSubtitleRowHeight}, 1, 0,
        [this](const int) { return rowTitle(selectedIndex_); },
        [this](const int) { return rowSubtitle(selectedIndex_); },
        [this](const int) { return rowIcon(selectedIndex_); });
  } else if (items_.empty()) {
    const int messageY = contentTop + (contentBottom - contentTop) / 3;
    GUI.drawHelpText(renderer, Rect{screen.x, messageY, screen.width, helpHeight},
                     storageError_ ? tr(STR_SAVED_ITEMS_READ_FAILED) : tr(STR_SAVED_BOOK_EMPTY));
    if (!storageError_) {
      GUI.drawHelpText(renderer, Rect{screen.x, messageY + helpHeight, screen.width, helpHeight},
                       tr(STR_SAVED_BOOK_EMPTY_HELP));
    }
  } else {
    GUI.drawList(
        renderer, Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)},
        static_cast<int>(items_.size()), tabFocused_ ? -1 : selectedIndex_,
        [this](const int index) { return rowTitle(index); }, [this](const int index) { return rowSubtitle(index); },
        [this](const int index) { return rowIcon(index); });
    GUI.drawHelpText(renderer, Rect{screen.x, contentBottom, screen.width, helpHeight},
                     storageError_ ? tr(STR_ERROR_GENERAL_FAILURE) : tr(STR_HOLD_OPEN_TO_DELETE));
  }

  const char* back = view_ == View::DeleteConfirm ? tr(STR_CANCEL) : (tabFocused_ ? tr(STR_BACK) : currentTabLabel());
  const char* confirm = "";
  if (view_ == View::DeleteConfirm) {
    confirm = tr(STR_DELETE);
  } else if (tabFocused_) {
    confirm = tabCount() > 1 ? tabItems[BookSavedItemsNavigation::nextTab(static_cast<uint8_t>(tab_), tabCount())].label
                             : (items_.empty() ? "" : tr(STR_SELECT));
  } else if (!items_.empty()) {
    confirm = tr(STR_OPEN);
  }
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
