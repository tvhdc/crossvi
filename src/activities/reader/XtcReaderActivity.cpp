/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>

#include <algorithm>
#include <cstdio>
#include <limits>

#include "BookSavedItemsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderMenuActivity.h"
#include "FinishedBooksStore.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ProgressFileCodec.h"
#include "ReaderUtils.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsDateEditActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {

enum class XtchRenderPass : uint8_t { Base, Lsb, Msb };

xtc::XtcError streamXtchRenderPass(const Xtc& book, const uint32_t page, const xtc::PageLayout& layout,
                                   const uint16_t pageWidth, const uint16_t pageHeight, const xtc::Viewport& viewport,
                                   GfxRenderer& renderer, const XtchRenderPass pass) {
  return book.loadPageStreaming(
      page,
      [&](const uint8_t* data, const size_t size, const size_t offset) {
        for (size_t index = 0; index < size; ++index) {
          bool secondPlane = false;
          uint16_t sourceX = 0;
          uint16_t sourceYBase = 0;
          if (!xtc::locateXthStreamByte(layout, pageWidth, pageHeight, offset + index, secondPlane, sourceX,
                                        sourceYBase)) {
            continue;
          }

          const xtc::CoordinateRange destinationX = xtc::mapSourceCoordinateRange(sourceX, pageWidth, viewport.width);
          for (uint8_t bit = 0; bit < 8 && sourceYBase + bit < pageHeight; ++bit) {
            if (((data[index] >> (7U - bit)) & 1U) == 0) continue;
            const xtc::CoordinateRange destinationY =
                xtc::mapSourceCoordinateRange(static_cast<uint16_t>(sourceYBase + bit), pageHeight, viewport.height);
            for (uint16_t y = destinationY.begin; y < destinationY.end; ++y) {
              for (uint16_t x = destinationX.begin; x < destinationX.end; ++x) {
                const int screenX = viewport.x + x;
                const int screenY = viewport.y + y;
                switch (pass) {
                  case XtchRenderPass::Base:
                    // The one-bit base is black when either XTH plane is set.
                    renderer.drawPixel(screenX, screenY, true);
                    break;
                  case XtchRenderPass::Lsb:
                    // White only for value 1: plane 0 sets, plane 1 clears.
                    renderer.drawPixel(screenX, screenY, secondPlane);
                    break;
                  case XtchRenderPass::Msb:
                    // White for values 1 and 2: plane 0 seeds, plane 1 toggles.
                    renderer.drawPixel(screenX, screenY,
                                       secondPlane ? !renderer.isPixelBlack(screenX, screenY) : false);
                    break;
                }
              }
            }
          }
        }
      },
      1024);
}

}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  // Fixed-layout books do not use the reflowable reader font.
  sdFontSystem.releaseLoadedFont(renderer);

  if (!xtc) {
    return;
  }
  xtc->setupCacheDir();

  const ReadingStatsCompletionTransaction::RecoveryResult completionRecovery =
      ReadingStatsCompletionTransaction::recoverPending();
  const bool completionStatsWritable = completionRecovery != ReadingStatsCompletionTransaction::RecoveryResult::Blocked;
  BookReadingStats::LoadStatus bookStatsStatus = BookReadingStats::LoadStatus::Missing;
  bookReadingStats = BookReadingStats::load(xtc->getCachePath(), &bookStatsStatus);
  bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(bookStatsStatus);
  bookReadingStatsWritable =
      completionStatsWritable && bookReadingStatsTrusted && BookReadingStats::canPublish(xtc->getCachePath());
  GlobalReadingStats::LoadStatus globalStatsStatus = GlobalReadingStats::LoadStatus::Missing;
  globalReadingStats = GlobalReadingStats::load(&globalStatsStatus);
  globalReadingStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(globalStatsStatus);
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
  completionAttemptBlocked = false;
  pendingStatsCompletionError = false;
  pendingReadingViewSignal.store(0, std::memory_order_relaxed);
  autoPageTurnSeconds = 0;
  automaticPageTurnActive = false;
  lastPageTurnTime = millis();
  confirmHold.reset();
  pageTurnGesture.reset();
  ignoreNextConfirmRelease = false;

  // Load saved progress
  loadProgress();
  if (initialBookmarkPage) {
    if (*initialBookmarkPage < xtc->getPageCount()) {
      currentPage = *initialBookmarkPage;
    } else {
      LOG_ERR("XTC", "Rejected out-of-range bookmark page: %lu", static_cast<unsigned long>(*initialBookmarkPage));
    }
    initialBookmarkPage.reset();
  }
  loadBookmarks();
  updateCurrentPageBookmarked();
  // Resolve immutable chapter metadata before the first render so the render
  // task and a fast Confirm press cannot race the parser's lazy-load state.
  if (xtc->hasChapters()) xtc->getChapters();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  if (!skipStartupRecentUpdate) {
    RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  }

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  commitReadingSession();
  saveReadingStats();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();

  sdFontSystem.releaseLoadedFont(renderer);

  if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
}

void XtcReaderActivity::onPause() {
  clearBlockingFeedback();
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  lastPageTurnTime = millis();
}

void XtcReaderActivity::onResume() {
  pendingReadingViewSignal.store(0, std::memory_order_release);
  lastPageTurnTime = millis();
  pageTurnGesture.reset();
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmHold.reset();
    ignoreNextConfirmRelease = false;
  }
}

void XtcReaderActivity::openReaderMenu() {
  if (!xtc || xtc->getPageCount() == 0) return;
  updateCurrentPageBookmarked();
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  uint32_t displayedPage = currentPage;
  if (lastSuccessfullyRenderedPage != std::numeric_limits<uint32_t>::max()) {
    displayedPage = lastSuccessfullyRenderedPage;
  }
  displayedPage = std::min(displayedPage, xtc->getPageCount() - 1);
  const int progressPercent =
      bookReadingStats.isCompleted
          ? 100
          : static_cast<int>((static_cast<uint64_t>(displayedPage) + 1) * 100 / xtc->getPageCount());
  const bool hasChapters = xtc->hasChapters() && !xtc->getChapters().empty();
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, xtc->getTitle(), static_cast<int>(displayedPage + 1),
          static_cast<int>(xtc->getPageCount()), progressPercent, SETTINGS.orientation, autoPageTurnSeconds,
          automaticPageTurnActive, false, !cachedBookmarks.empty(), currentPageBookmarked,
          EpubReaderMenuActivity::ReaderKind::FixedLayout, false, false, hasChapters, bookReadingStats.isCompleted),
      [this](const ActivityResult& result) {
        const auto* menu = std::get_if<MenuResult>(&result.data);
        if (!menu) {
          requestUpdate();
          return;
        }
        if (menu->autoPageTurnChanged) {
          autoPageTurnSeconds = menu->autoPageTurnSeconds;
          automaticPageTurnActive = autoPageTurnSeconds != 0;
        }
        lastPageTurnTime = millis();
        if (!result.isCancelled) {
          handleReaderMenuAction(menu->action);
        } else {
          requestUpdate();
        }
      });
}

void XtcReaderActivity::openGoToPage() {
  if (!xtc || xtc->getPageCount() == 0) return;
  const uint32_t pageCount = xtc->getPageCount();
  const uint32_t initial = std::min(currentPage, pageCount - 1) + 1;
  startActivityForResult(std::make_unique<IntervalSelectionActivity>(
                             renderer, mappedInput, "XtcGoToPage", StrId::STR_GO_TO_PAGE, static_cast<int>(initial), 1,
                             static_cast<int>(pageCount), 1, 10, StrId::STR_PAGE_NUMBER_FORMAT, true, true),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const uint32_t selected = std::get<IntervalResult>(result.data).value;
                             RenderLock lock(*this);
                             if (xtc && selected > 0 && selected <= xtc->getPageCount()) currentPage = selected - 1;
                           }
                           lastPageTurnTime = millis();
                           requestUpdate();
                         });
}

void XtcReaderActivity::confirmMarkBookCompleted() {
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_BOOK_COMPLETE),
                                                                tr(STR_MARK_BOOK_COMPLETE_CONFIRM)),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) markBookCompleted();
                           requestUpdate();
                         });
}

void XtcReaderActivity::handleReaderMenuAction(const int action) {
  switch (static_cast<EpubReaderMenuActivity::MenuAction>(action)) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER:
      openChapterSelection();
      break;
    case EpubReaderMenuActivity::MenuAction::READING_STATS:
      openReadingStats();
      break;
    case EpubReaderMenuActivity::MenuAction::SAVED_ITEMS:
      openSavedItems();
      break;
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK:
      showBookmarkMessage = toggleBookmark();
      if (showBookmarkMessage) bookmarkMessageTime = millis();
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PAGE:
      openGoToPage();
      break;
    case EpubReaderMenuActivity::MenuAction::MARK_COMPLETE:
      confirmMarkBookCompleted();
      break;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      break;
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      requestUpdate();
      break;
    default:
      requestUpdate();
      break;
  }
}

void XtcReaderActivity::openChapterSelection() {
  std::shared_ptr<Xtc> book;
  uint32_t page = 0;
  {
    RenderLock lock;
    book = xtc;
    page = currentPage;
  }
  if (book && book->hasChapters() && !book->getChapters().empty()) {
    startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, book, page),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               RenderLock lock;
                               currentPage = std::get<PageResult>(result.data).page;
                             }
                           });
  }
}

void XtcReaderActivity::loop() {
  consumeReadingViewSignal();
  if (readingSessionTracker.discardIfIdle(static_cast<uint32_t>(millis()))) {
    hasActiveReadingSpanStartLocalDateTime = false;
    LOG_DBG("XRS", "Reading interval discarded after idle threshold");
  }

  if (!xtc) {
    return;
  }
  if (showBookmarkMessage && millis() - bookmarkMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  uint32_t pageSnapshot = 0;
  {
    RenderLock lock;
    pageSnapshot = currentPage;
  }
  const bool atEndOfBook = pageSnapshot >= xtc->getPageCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHold.onPress();
    if (!automaticPageTurnActive && !(atEndOfBook && endOfBookOptions.menuActive())) {
      queueBlockingFeedback(SETTINGS.longPressMenuFunction == CrossPointSettings::LP_MENU_DISABLED
                                ? StrId::STR_OPENING_READER_MENU
                                : StrId::STR_PROCESSING);
    }
  }
  const bool suppressConfirmRelease =
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) && ignoreNextConfirmRelease;
  if (suppressConfirmRelease) {
    confirmHold.onRelease();
    ignoreNextConfirmRelease = false;
  }

  if (automaticPageTurnActive) {
    if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !suppressConfirmRelease) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    const unsigned long autoPageTurnInterval = static_cast<unsigned long>(autoPageTurnSeconds) * 1000UL;
    if (!atEndOfBook && autoPageTurnSeconds != 0 && millis() - lastPageTurnTime >= autoPageTurnInterval) {
      bool pageTurned = false;
      {
        RenderLock lock(*this);
        const uint32_t lockedPage = currentPage;
        const uint32_t pageCount = xtc->getPageCount();
        if (lockedPage < pageCount && lastSuccessfullyRenderedPage == lockedPage &&
            millis() - lastPageTurnTime >= autoPageTurnInterval) {
          consumeReadingViewSignal();
          stopReadingPage(true, static_cast<uint32_t>(millis()));
          if (lockedPage + 1 >= pageCount) {
            markBookCompleted();
            if (!pendingStatsCompletionError && bookReadingStats.isCompleted) currentPage = pageCount;
            automaticPageTurnActive = false;
          } else {
            currentPage = lockedPage + 1;
            refreshEstimatedTimeLeft();
          }
          lastPageTurnTime = millis();
          pageTurned = true;
        }
      }
      if (pageTurned) {
        requestUpdate();
        return;
      }
    }
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block.
  bool endOfBookMenuActive = false;
  EndOfBookOptions::Action endOfBookAction = EndOfBookOptions::Action::None;
  std::string openPath;
  if (atEndOfBook) {
    RenderLock lock;
    endOfBookMenuActive = endOfBookOptions.menuActive();
    if (endOfBookMenuActive && !suppressConfirmRelease &&
        !ReaderUtils::isLongPageTurnRelease(mappedInput, pageTurnGesture)) {
      endOfBookAction = endOfBookOptions.handleMenuInput(mappedInput, &openPath);
    }
  }
  if (endOfBookMenuActive) {
    switch (endOfBookAction) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage: {
        RenderLock lock;
        currentPage = xtc->getPageCount() > 0 ? xtc->getPageCount() - 1 : 0;
      }
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  if (SETTINGS.longPressMenuFunction != CrossPointSettings::LP_MENU_DISABLED &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      confirmHold.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Confirm), ReaderUtils::CONFIRM_HOLD_MS)) {
    ignoreNextConfirmRelease = true;
    clearBlockingFeedback();
    if (handleReaderShortcut(SETTINGS.longPressMenuFunction)) return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (suppressConfirmRelease) return;
    if (confirmHold.onRelease() == ReaderUtils::HoldRelease::Short) {
      openReaderMenu();
    }
    return;
  }

  if (ReaderUtils::handleBackNavigation(
          mappedInput, activityManager, xtc ? xtc->getPath().c_str() : "",
          {nullptr, [](void*) { activityManager.returnFromReaderOrHome(); }},
          {this, [](void* ctx) { static_cast<XtcReaderActivity*>(ctx)->showReaderExitFeedback(); }})) {
    return;
  }

  const auto pageGesture = ReaderUtils::detectPageTurnGesture(mappedInput, pageTurnGesture);
  const bool prevTriggered = pageGesture.prev;
  const bool nextTriggered = pageGesture.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (pageSnapshot >= xtc->getPageCount()) {
    if (endOfBookMenuActive) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      {
        RenderLock lock;
        currentPage = xtc->getPageCount() - 1;
      }
      requestUpdate();
    }
    return;
  }

  if (pageGesture.longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    pendingShortcutUnsupportedNotice = true;
    requestUpdate();
    return;
  }

  const int skipAmount = pageGesture.longPress ? 10 : 1;

  if (prevTriggered) {
    {
      RenderLock lock;
      consumeReadingViewSignal();
      stopReadingPage(false, static_cast<uint32_t>(millis()));
      if (currentPage >= static_cast<uint32_t>(skipAmount)) {
        currentPage -= skipAmount;
      } else {
        currentPage = 0;
      }
    }
    requestUpdate();
  } else if (nextTriggered) {
    bool completionFailed = false;
    {
      RenderLock lock;
      consumeReadingViewSignal();
      stopReadingPage(true, static_cast<uint32_t>(millis()), !pageGesture.longPress);
      const uint32_t pageCount = xtc->getPageCount();
      const uint64_t requested = static_cast<uint64_t>(currentPage) + static_cast<uint32_t>(skipAmount);
      if (requested >= pageCount) {
        if (pageCount > 0 && lastSuccessfullyRenderedPage == pageCount - 1) {
          markBookCompleted();
          completionFailed = pendingStatsCompletionError;
          if (!completionFailed) {
            currentPage = pageCount;
            automaticPageTurnActive = false;
          }
        } else {
          currentPage = pageCount > 0 ? pageCount - 1 : 0;
        }
      } else {
        currentPage = static_cast<uint32_t>(requested);
        refreshEstimatedTimeLeft();
      }
    }
    if (completionFailed) return;
    requestUpdate();
  }
}

bool XtcReaderActivity::handleReaderShortcut(const uint8_t function) {
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_FUNCTION>(function)) {
    case CrossPointSettings::LP_MENU_READING_STATS:
      openReadingStats();
      return true;
    case CrossPointSettings::LP_MENU_AUTO_PAGE_TURN:
      autoPageTurnSeconds = ReaderUtils::autoPageTurnShortcutSeconds(autoPageTurnSeconds);
      automaticPageTurnActive = !automaticPageTurnActive;
      lastPageTurnTime = millis();
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_BOOKMARK:
      showBookmarkMessage = toggleBookmark();
      if (showBookmarkMessage) bookmarkMessageTime = millis();
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_KOSYNC:
    case CrossPointSettings::LP_MENU_DICTIONARY:
    case CrossPointSettings::LP_MENU_HIGHLIGHT:
      pendingShortcutUnsupportedNotice = true;
      requestUpdate();
      return true;
    case CrossPointSettings::LP_MENU_DISABLED:
      return false;
    default:
      pendingShortcutUnsupportedNotice = true;
      requestUpdate();
      return true;
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (renderReaderExitOverlay()) return;
  if (renderBlockingFeedbackOverlay()) return;
  const std::shared_ptr<Xtc> book = xtc;
  const uint32_t page = currentPage;
  if (!book) {
    return;
  }

  // Bounds check
  if (page >= book->getPageCount()) {
    signalReadingPageHidden();
    // Show end of book screen. Sole load site: runs on the render task (serialized by
    // RenderLock); the main task only reads the suggestions once the flag is published.
    endOfBookOptions.loadOnce(book->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    if (pendingBookmarkStorageError.exchange(false)) {
      GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
    } else if (pendingShortcutUnsupportedNotice.exchange(false)) {
      GUI.drawPopup(renderer, tr(STR_SHORTCUT_NOT_SUPPORTED));
    }
    renderer.displayBuffer();
    if (pendingScreenshot.exchange(false)) {
      ScreenshotUtil::takeScreenshot(renderer);
    }
    return;
  }

  if (renderPage(book, page)) {
    lastSuccessfullyRenderedPage = page;
    lastPageTurnTime = millis();
    signalReadingPageVisible();
    if (page != lastSavedPage && saveProgress(book, page)) lastSavedPage = page;
    if (pendingScreenshot.exchange(false)) {
      ScreenshotUtil::takeScreenshot(renderer);
    }
  } else {
    lastSuccessfullyRenderedPage = std::numeric_limits<uint32_t>::max();
    signalReadingPageHidden();
  }
  if (pendingStatsCompletionError.exchange(false)) {
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_COMPLETE_BOOK_STATS_FAILED));
  } else if (pendingBookmarkStorageError.exchange(false)) {
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  } else if (pendingShortcutUnsupportedNotice.exchange(false)) {
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_SHORTCUT_NOT_SUPPORTED));
  } else if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

void XtcReaderActivity::loadBookmarks() {
  cachedBookmarks.clear();
  bookmarksWritable = false;
  if (!xtc) return;

  const std::string canonicalPath = BookmarkUtil::getBookmarkPath(xtc->getPath());
  const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(xtc->getPath());
  const std::string path = BookmarkUtil::canonicalFamilyExists(xtc->getPath()) ? canonicalPath : legacyPath;
  BookmarkBookMetadata metadata;
  const auto status = JsonSettingsIO::loadBookmarksFromFile(cachedBookmarks, path.c_str(), &metadata);
  bookmarksWritable =
      status == JsonSettingsIO::BookmarkLoadStatus::Loaded || status == JsonSettingsIO::BookmarkLoadStatus::Missing;
  if (bookmarksWritable &&
      !BookmarkUtil::metadataMatchesBook(metadata, xtc->getPath(), BookmarkEntry::PositionKind::FixedLayout)) {
    bookmarksWritable = false;
  }
  if (bookmarksWritable) {
    bookmarksWritable = std::all_of(cachedBookmarks.begin(), cachedBookmarks.end(), [this](const BookmarkEntry& entry) {
      return entry.positionKind == BookmarkEntry::PositionKind::FixedLayout && entry.pageIndex < xtc->getPageCount();
    });
  }
  if (!bookmarksWritable) {
    cachedBookmarks.clear();
    LOG_ERR("XTR", "Bookmark state is not writable after load failure (%u)", static_cast<unsigned>(status));
  } else if (status == JsonSettingsIO::BookmarkLoadStatus::Loaded && metadata.path.empty()) {
    metadata = {xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), "xtc"};
    if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, canonicalPath.c_str(), &metadata)) {
      LOG_ERR("XTR", "Could not add book metadata to legacy bookmarks");
    }
  }
}

void XtcReaderActivity::updateCurrentPageBookmarked() {
  uint32_t page = currentPage;
  if (lastSuccessfullyRenderedPage != std::numeric_limits<uint32_t>::max()) page = lastSuccessfullyRenderedPage;
  currentPageBookmarked =
      std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [page](const BookmarkEntry& bookmark) {
        return bookmark.positionKind == BookmarkEntry::PositionKind::FixedLayout && bookmark.pageIndex == page;
      });
}

bool XtcReaderActivity::toggleBookmark() {
  if (!xtc || !bookmarksWritable || xtc->getPageCount() == 0) {
    pendingBookmarkStorageError = true;
    return false;
  }
  uint32_t page = currentPage;
  if (lastSuccessfullyRenderedPage != std::numeric_limits<uint32_t>::max()) page = lastSuccessfullyRenderedPage;
  if (page >= xtc->getPageCount()) return false;

  const std::vector<BookmarkEntry> previous = cachedBookmarks;
  const auto existing =
      std::find_if(cachedBookmarks.begin(), cachedBookmarks.end(), [page](const BookmarkEntry& entry) {
        return entry.positionKind == BookmarkEntry::PositionKind::FixedLayout && entry.pageIndex == page;
      });
  bookmarkRemoved = existing != cachedBookmarks.end();
  if (bookmarkRemoved) {
    cachedBookmarks.erase(existing);
  } else {
    BookmarkEntry bookmark;
    bookmark.positionKind = BookmarkEntry::PositionKind::FixedLayout;
    bookmark.pageIndex = page;
    bookmark.percentage = static_cast<float>(page + 1) / xtc->getPageCount();
    char summary[32]{};
    std::snprintf(summary, sizeof(summary), tr(STR_PAGE_NUMBER_FORMAT), static_cast<unsigned>(page + 1));
    bookmark.summary = summary;
    cachedBookmarks.push_back(std::move(bookmark));
  }

  const std::string directory = BookmarkUtil::getBookmarksDir();
  const std::string path = BookmarkUtil::getBookmarkPath(xtc->getPath());
  const BookmarkBookMetadata metadata{xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), "xtc"};
  if ((!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str())) ||
      !JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str(), &metadata)) {
    cachedBookmarks = previous;
    pendingBookmarkStorageError = true;
    return false;
  }
  updateCurrentPageBookmarked();
  return true;
}

void XtcReaderActivity::openSavedItems() {
  if (!xtc) return;
  startActivityForResult(std::make_unique<BookSavedItemsActivity>(
                             renderer, mappedInput, xtc->getPath(), xtc->getTitle(), xtc->getAuthor(),
                             BookSavedItemsActivity::ReaderKind::FixedLayout, nullptr, xtc->getPageCount()),
                         [this](const ActivityResult& result) {
                           loadBookmarks();
                           updateCurrentPageBookmarked();
                           if (!result.isCancelled) {
                             const auto* page = std::get_if<PageResult>(&result.data);
                             if (page && xtc && page->page < xtc->getPageCount()) {
                               RenderLock lock(*this);
                               currentPage = page->page;
                             }
                           }
                           requestUpdate();
                         });
}

void XtcReaderActivity::signalReadingPageVisible() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(1, std::memory_order_release);
}

void XtcReaderActivity::signalReadingPageHidden() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(-1, std::memory_order_release);
}

void XtcReaderActivity::consumeReadingViewSignal() {
  const int8_t signal = pendingReadingViewSignal.exchange(0, std::memory_order_acq_rel);
  if (signal == 0) return;

  const uint32_t eventAtMs = pendingReadingViewAtMs.load(std::memory_order_relaxed);
  if (signal > 0) {
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

void XtcReaderActivity::recordReadingSample(const ReadingSessionSample& sample, const bool recordPace) {
  if (sample.seconds > 0) sessionReadingSeconds = addReadingStatsSaturated(sessionReadingSeconds, sample.seconds);
  if (!sample.forwardPageRead) return;

  if (bookReadingStatsWritable) {
    bookReadingStats.totalPagesTurned = addReadingStatsSaturated(bookReadingStats.totalPagesTurned, 1);
    if (recordPace) bookReadingStats.recordForwardPageRead(sample.seconds);
    bookReadingStatsDirty = true;
  }
  if (globalReadingStatsWritable) {
    globalReadingStats.totalPagesTurned = addReadingStatsSaturated(globalReadingStats.totalPagesTurned, 1);
    globalReadingStatsDirty = true;
  }
}

void XtcReaderActivity::stopReadingPage(const bool forwardPageTurn, const uint32_t nowMs, const bool recordPace) {
  const ReadingSessionSample sample = readingSessionTracker.stop(nowMs, forwardPageTurn);
  if (sample.seconds > 0 && hasActiveReadingSpanStartLocalDateTime) {
    pendingBookReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
    pendingGlobalReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
  }
  hasActiveReadingSpanStartLocalDateTime = false;
  recordReadingSample(sample, recordPace);
}

bool XtcReaderActivity::refreshEstimatedTimeLeft() {
  if (!bookReadingStatsWritable || !xtc) return false;
  if (bookReadingStats.isCompleted) {
    if (bookReadingStats.estimatedTimeLeftSeconds != 0) {
      bookReadingStats.estimatedTimeLeftSeconds = 0;
      bookReadingStatsDirty = true;
    }
    return false;
  }
  if (bookReadingStats.paceSampleCount < 3 || bookReadingStats.avgSecondsPerForwardPage == 0) return false;
  const uint32_t estimate = estimateRemainingReadingSeconds(
      xtc->getPageCount(), currentPage, bookReadingStats.avgSecondsPerForwardPage, bookReadingStats.paceSampleCount);
  if (estimate != bookReadingStats.estimatedTimeLeftSeconds) {
    bookReadingStats.estimatedTimeLeftSeconds = estimate;
    bookReadingStatsDirty = true;
  }
  return estimate != 0;
}

void XtcReaderActivity::commitReadingSession() {
  if (readingSessionCommitted) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  readingSessionCommitted = true;

  // Match the released CrossInk-compatible contract used by EPUB/TXT: ten
  // active seconds contribute time, one minute counts as a session.
  if (sessionReadingSeconds >= 60) {
    if (bookReadingStatsWritable) {
      if (bookReadingStats.sessionCount < std::numeric_limits<uint16_t>::max()) ++bookReadingStats.sessionCount;
      bookReadingStatsDirty = true;
    }
    if (globalReadingStatsWritable) {
      globalReadingStats.totalSessions = addReadingStatsSaturated(globalReadingStats.totalSessions, 1);
      if (hasSessionStartLocalDateTime) globalReadingStats.recordReadingSession(sessionStartLocalDateTime.date);
      globalReadingStatsDirty = true;
    }
  }
  if (sessionReadingSeconds < 10) return;

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

void XtcReaderActivity::saveReadingStats() {
  if (bookReadingStatsWritable && bookReadingStatsDirty && xtc) {
    if (bookReadingStats.save(xtc->getCachePath())) {
      bookReadingStatsDirty = false;
    } else {
      LOG_ERR("XRS", "Failed to save book reading statistics");
    }
  }
  if (globalReadingStatsWritable && globalReadingStatsDirty) {
    if (globalReadingStats.save()) {
      globalReadingStatsDirty = false;
    } else {
      LOG_ERR("XRS", "Failed to save global reading statistics");
    }
  }
}

void XtcReaderActivity::markBookCompleted() {
  if (!xtc || bookReadingStats.isCompleted || completionAttemptBlocked) return;
  const auto reportFailure = [this]() {
    completionAttemptBlocked = true;
    pendingStatsCompletionError = true;
    requestUpdate();
  };
  if (!bookReadingStatsWritable || !globalReadingStatsWritable) {
    LOG_ERR("XRS", "Could not mark the book complete because reading statistics are protected or unreadable");
    reportFailure();
    return;
  }

  saveReadingStats();
  if (bookReadingStatsDirty || globalReadingStatsDirty) {
    LOG_ERR("XRS", "Could not flush reading statistics before marking the book complete");
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
  if (!ReadingStatsCompletionTransaction::commit(xtc->getCachePath(), bookReadingStats, completedBookStats,
                                                 globalReadingStats, completedGlobalStats)) {
    LOG_ERR("XRS", "Could not commit book completion statistics");
    bookReadingStatsWritable = false;
    globalReadingStatsWritable = false;
    reportFailure();
    return;
  }
  bookReadingStats = completedBookStats;
  globalReadingStats = completedGlobalStats;
  FINISHED_BOOKS.markCompleted(
      xtc->getPath(), xtc->getTitle(), xtc->getAuthor(),
      completedBookStats.finishedDate.isValid() ? readingStatsDayIndex(completedBookStats.finishedDate) : 0);
}

void XtcReaderActivity::openReadingStats() {
  if (!xtc) return;

  BookReadingStats displayBookStats;
  GlobalReadingStats displayDeviceStats;
  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  {
    RenderLock lock(*this);
    consumeReadingViewSignal();
    stopReadingPage(false, static_cast<uint32_t>(millis()));
    displayBookStats = bookReadingStats;
    displayDeviceStats = globalReadingStats;
    if (!readingSessionCommitted) {
      previewReadingStatsSession(bookReadingStatsWritable ? &displayBookStats : nullptr,
                                 globalReadingStatsWritable ? &displayDeviceStats : nullptr, sessionReadingSeconds,
                                 pendingBookReadingSpans, pendingGlobalReadingSpans,
                                 hasSessionStartLocalDateTime ? &sessionStartLocalDateTime : nullptr);
    }

    if (bookReadingStatsTrusted && bookReadingStats.isCompleted) {
      progress = ReadingStatsMetric::known(100);
    } else if (lastSuccessfullyRenderedPage != std::numeric_limits<uint32_t>::max() && xtc->getPageCount() > 0) {
      const uint64_t completed = static_cast<uint64_t>(lastSuccessfullyRenderedPage) + 1;
      progress = ReadingStatsMetric::known(
          static_cast<uint32_t>(std::min<uint64_t>(100, completed * 100 / xtc->getPageCount())));
    }
  }

  const GlobalReadingStatsAggregation allSyncedStats = GlobalReadingStats::loadAggregatedWithReport(displayDeviceStats);
  ReadingStatsDateTime now;
  const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  ReadingStatsPresentation presentation =
      buildReadingStatsPresentation(displayBookStats, bookReadingStatsTrusted, displayDeviceStats,
                                    globalReadingStatsTrusted, allSyncedStats, currentDateTime, progress, false);
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, xtc->getTitle(), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, bookReadingStatsWritable, false),
      [this](const ActivityResult& result) {
        const auto* action = std::get_if<ReadingStatsActionResult>(&result.data);
        if (!action || action->action != ReadingStatsActionResult::Action::EditBookDates || !xtc ||
            !bookReadingStatsWritable) {
          requestUpdate();
          return;
        }
        const std::string cachePath = xtc->getCachePath();
        startActivityForResult(
            std::make_unique<ReadingStatsDateEditActivity>(renderer, mappedInput, cachePath, bookReadingStats),
            [this, cachePath](const ActivityResult& editResult) {
              if (!editResult.isCancelled) {
                BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
                bookReadingStats = BookReadingStats::load(cachePath, &status);
                bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(status);
                bookReadingStatsWritable = bookReadingStatsTrusted && BookReadingStats::canPublish(cachePath);
                bookReadingStatsDirty = false;
              }
              requestUpdate();
            });
      });
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo(const std::shared_ptr<Xtc>& book,
                                                                     const uint32_t page) const {
  const int bookPageCount = static_cast<int>(book->getPageCount());
  const int bookPage = static_cast<int>(page) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? book->getTitle() : "";

  if (!book->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = book->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [page](const xtc::ChapterInfo& chapter) {
    return page >= chapter.startPage && page <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(page - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const std::shared_ptr<Xtc>& book, const uint32_t page,
                                               const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(book->getPageCount());
  const int displayPage = static_cast<int>(page) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo(book, page);
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

bool XtcReaderActivity::renderPage(const std::shared_ptr<Xtc>& book, const uint32_t page) {
  const uint16_t pageWidth = book->getPageWidth();
  const uint16_t pageHeight = book->getPageHeight();
  const uint8_t bitDepth = book->getBitDepth();

  xtc::PageLayout pageLayout;
  if (!xtc::calculatePageLayout(pageWidth, pageHeight, bitDepth, pageLayout)) return false;

  xtc::Viewport viewport;
  if (!xtc::calculateFitViewport(pageWidth, pageHeight, renderer.getScreenWidth(), renderer.getScreenHeight(),
                                 viewport)) {
    return false;
  }

  const auto renderStatusBar = [&] {
    if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
      renderStatusBarOverlay(book, page, StatusBarOverlayPosition::Top);
    } else {
      renderStatusBarOverlay(book, page, StatusBarOverlayPosition::Bottom);
    }
  };

  if (bitDepth == 2) {
    const auto showStreamError = [&](const xtc::XtcError error) {
      LOG_ERR("XTR", "Failed to stream XTCH page %lu: %s", page, xtc::errorToString(error));
      renderer.clearScreen();
      const char* message = error == xtc::XtcError::MEMORY_ERROR ? tr(STR_MEMORY_ERROR) : tr(STR_PAGE_LOAD_ERROR);
      renderer.drawCenteredText(UI_12_FONT_ID, 300, message, true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
    };

    // XTCH is two roughly 48 KiB bit planes. Stream each rendering pass
    // through the parser's fixed 1 KiB chunk instead of requiring one
    // contiguous ~96 KiB allocation on the ESP32-C3 heap.
    renderer.clearScreen();
    xtc::XtcError streamError =
        streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer, XtchRenderPass::Base);
    if (streamError != xtc::XtcError::OK) {
      showStreamError(streamError);
      return false;
    }
    renderStatusBar();

    if (!renderer.supportsStripGrayscale()) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit as 1-bit)", page + 1, book->getPageCount());
      return true;
    }

    if (pagesUntilFullRefresh <= 1) {
      // Periodic ghost cleanup: scrub via the normal path, then run the
      // settle flavor of the grayscale base pass (DTM planes are equal after
      // the display sync, so only the gentle reinforcement cells fire).
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      // OEM grayscale pipeline base: differential "AA-pre-BW(mid)" update as
      // the page turn on X3; plain FAST refresh on X4 (previous behavior).
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    streamError =
        streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer, XtchRenderPass::Lsb);
    if (streamError != xtc::XtcError::OK) {
      showStreamError(streamError);
      return false;
    }
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    streamError =
        streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer, XtchRenderPass::Msb);
    if (streamError != xtc::XtcError::OK) {
      showStreamError(streamError);
      return false;
    }
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();

    renderer.clearScreen();
    streamError =
        streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer, XtchRenderPass::Base);
    if (streamError != xtc::XtcError::OK) {
      showStreamError(streamError);
      return false;
    }
    renderStatusBar();
    renderer.cleanupGrayscaleWithFrameBuffer();

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", page + 1, book->getPageCount());
    return true;
  }

  const size_t pageBufferSize = pageLayout.payloadBytes;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return false;
  }
  const size_t bytesRead = book->loadPage(page, pageBuffer, pageBufferSize);
  if (bytesRead != pageBufferSize) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", page, pageBufferSize, bitDepth,
            xtc::errorToString(book->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return false;
  }

  renderer.clearScreen();
  const size_t sourceRowBytes = pageLayout.rowBytes;
  for (uint16_t destinationY = 0; destinationY < viewport.height; ++destinationY) {
    const uint16_t sourceY = xtc::mapViewportCoordinate(destinationY, viewport.height, pageHeight);
    const size_t sourceRowStart = sourceY * sourceRowBytes;
    for (uint16_t destinationX = 0; destinationX < viewport.width; ++destinationX) {
      const uint16_t sourceX = xtc::mapViewportCoordinate(destinationX, viewport.width, pageWidth);
      const size_t sourceByte = sourceRowStart + sourceX / 8U;
      const uint8_t sourceBit = static_cast<uint8_t>(7U - sourceX % 8U);
      if (((pageBuffer[sourceByte] >> sourceBit) & 1U) == 0) {
        renderer.drawPixel(viewport.x + destinationX, viewport.y + destinationY, true);
      }
    }
  }
  free(pageBuffer);
  renderStatusBar();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", page + 1, book->getPageCount(), bitDepth);
  return true;
}

bool XtcReaderActivity::saveProgress(const std::shared_ptr<Xtc>& book, const uint32_t page) const {
  uint8_t data[4];
  ProgressFileCodec::encodePage(page, data);
  const ProgressFile::PageBounds bounds{book->getPageCount()};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
  if (!ProgressFile::writeAtomic(book->getCachePath(), data, sizeof(data), validator)) {
    LOG_ERR("XTR", "Failed to save progress: page %lu", page);
    return false;
  }
  return true;
}

void XtcReaderActivity::loadProgress() {
  uint8_t data[4]{};
  const ProgressFile::PageBounds bounds{xtc->getPageCount()};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
  const ProgressFile::LoadResult progress = ProgressFile::loadPage(xtc->getCachePath(), data, sizeof(data), validator);
  if (progress) {
    const uint32_t loadedPage = ProgressFileCodec::decodePage(data);
    {
      RenderLock lock;
      currentPage = loadedPage;
      lastSavedPage = loadedPage;
    }
    LOG_DBG("XTR", "Loaded progress: page %lu", loadedPage);
  } else if (progress.source == ProgressFile::LoadSource::Invalid ||
             progress.source == ProgressFile::LoadSource::IoError) {
    LOG_ERR("XTR", "No valid progress copy could be read");
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
