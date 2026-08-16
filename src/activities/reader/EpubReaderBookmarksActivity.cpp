#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <util/BookmarkUtil.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

// Layout constants used in renderScreen
constexpr int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  if (!textMode && !epub) {
    return;
  }

  const std::string canonicalPath = BookmarkUtil::getBookmarkPath(bookPath);
  const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(bookPath);
  const std::string path = BookmarkUtil::canonicalFamilyExists(bookPath) ? canonicalPath : legacyPath;
  const JsonSettingsIO::BookmarkLoadStatus loaded =
      JsonSettingsIO::loadBookmarksFromFile(bookmarks, path.c_str(), &bookmarkMetadata);
  if (loaded == JsonSettingsIO::BookmarkLoadStatus::Loaded || loaded == JsonSettingsIO::BookmarkLoadStatus::Missing) {
    const bool matchingMetadata = BookmarkUtil::metadataMatchesBook(
        bookmarkMetadata, bookPath, textMode ? BookmarkEntry::PositionKind::Text : BookmarkEntry::PositionKind::Epub);
    const bool matchingKinds = std::all_of(bookmarks.begin(), bookmarks.end(), [this](const BookmarkEntry& bookmark) {
      return textMode ? bookmark.positionKind == BookmarkEntry::PositionKind::Text
                      : bookmark.positionKind == BookmarkEntry::PositionKind::Epub;
    });
    bookmarksWritable = matchingMetadata && matchingKinds;
    if (!bookmarksWritable) {
      bookmarks.clear();
      storageError = true;
    }
  } else {
    bookmarks.clear();
    bookmarks.shrink_to_fit();
    bookmarksWritable = false;
    storageError = true;
    LOG_ERR("EPB", "Bookmark state is not writable after load failure (%u)", static_cast<unsigned>(loaded));
  }
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), bookPath.c_str());
  if (bookmarkMetadata.path.empty()) {
    bookmarkMetadata.path = bookPath;
    bookmarkMetadata.bookType = textMode ? "txt" : "epub";
    if (epub) {
      bookmarkMetadata.title = epub->getTitle();
      bookmarkMetadata.author = epub->getAuthor();
    }
  }

  // Trigger first update
  requestUpdate();
}

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;  // Reserve vertical space for button hints at the bottom
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;  // Reserve vertical space for title and button hints
}

void EpubReaderBookmarksActivity::loop() {
  // Delete confirmation mode
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;  // first confirmation, update text
        requestUpdate();
        return;
      }
      if (!bookmarksWritable) {
        confirmingDelete = DELETE_MODE_OFF;
        storageError = true;
        requestUpdate();
        return;
      }
      const std::vector<BookmarkEntry> previousBookmarks = bookmarks;
      bookmarks.erase(bookmarks.begin() + selectorIndex);
      const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
      const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
      if ((!Storage.exists(bookmarksDir.c_str()) && !Storage.mkdir(bookmarksDir.c_str())) ||
          !JsonSettingsIO::saveBookmarks(bookmarks, path.c_str(), &bookmarkMetadata)) {
        LOG_ERR("EPB", "Failed to save bookmarks after delete");
        bookmarks = previousBookmarks;
        storageError = true;
        confirmingDelete = DELETE_MODE_OFF;
        requestUpdate();
        return;
      }

      // Move selector up if we deleted the last item
      if (selectorIndex >= bookmarks.size() && selectorIndex > 0) {
        selectorIndex--;
      }

      if (bookmarks.empty()) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
        return;
      }

      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    }
    // The confirmation dialog is modal. Ignore navigation and held-button
    // gestures until it is confirmed or dismissed.
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {  // Open
    if (bookmarks.empty()) {
      return;
    }
    auto bookmark = bookmarks.at(selectorIndex);
    ProgressChangeResult result{};
    result.xpath = bookmark.xpath;
    result.percentage = bookmark.percentage;
    result.hasSavedProgress = true;
    result.spineIndex = bookmark.computedSpineIndex;
    result.contentSourceOffset = bookmark.contentSourceOffset;
    result.hasContentSourceOffset = bookmark.hasContentSourceOffset;
    if (textMode) {
      result.textByteOffset = bookmark.byteOffset;
      result.hasTextByteOffset = true;
    } else if (bookmark.computedChapterPageCount > 0 &&
               bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
               bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
      result.page = bookmark.computedChapterProgress;
      result.totalPages = bookmark.computedChapterPageCount;
    }
    setResult(std::move(result));
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Confirm) > ENTER_DELETE_MODE_MS) {
    if (bookmarks.empty()) {
      return;
    }
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;  // Reserve vertical space for title
  const int listHeight = getListHeight(renderer);
  const int numBookmarks = bookmarks.size();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) {
    return bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index).summary;
  };
  const auto getBookmarkSubtitle = [this](int index) {
    auto bookmark = bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index);
    std::string subtitle = std::to_string((int)(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "%";
    if (textMode) return subtitle;
    auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    subtitle += " - ";
    if (bookmark.computedChapterPageCount > 0) {
      subtitle += std::to_string(bookmark.computedChapterProgress + 1) + "/" +
                  std::to_string(bookmark.computedChapterPageCount) + " - ";
    }
    return subtitle + tocTitle;
  };
  const auto getBookmarkIcon = [isPortrait](int index) {
    // only enabled icon in portrait mode due to limitation with rotating icons for other orientations
    return isPortrait ? UIIcon::Bookmark : UIIcon::None;
  };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));

      // render list with just the selected item for the user to confirm to delete
      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_OPEN_TO_DELETE));
    }
  }

  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      bookmarks.size() > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_SELECT)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (storageError) {
    storageError = false;
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}
