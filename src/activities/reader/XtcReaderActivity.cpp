/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t DEFERRED_COVER_IDLE_MS = 2000;

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
  const uint32_t readerStateStartedMs = static_cast<uint32_t>(millis());
  if (!xtc->setupCacheDir()) pendingBookmarkStorageError = true;
  progressWriteSession.invalidate();

  BookReadingStats::LoadStatus bookStatsStatus = BookReadingStats::LoadStatus::Missing;
  bookReadingStats = BookReadingStats::load(xtc->getCachePath(), &bookStatsStatus);
  bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(bookStatsStatus);
  // The writer repeats the fail-closed storage guard at the real publication
  // boundary, so a second eager cache scan only delays first paint.
  bookReadingStatsWritable = completionStatsWritableAtOpen && bookReadingStatsTrusted;
  globalReadingStats = {};
  globalReadingStatsTrusted = false;
  globalReadingStatsWritable = false;
  deferredOpenStatePending = true;
  deferredOpenStateReady = false;
  deferredGlobalPageTurns = 0;
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
  deferredCoverLastInputAt = static_cast<uint32_t>(millis());
  confirmHold.reset();
  pageTurnGesture.reset();
  pendingPageTurnDelta = 0;
  ignoreNextConfirmRelease = false;
  cachedBookmarks.clear();
  bookmarksLoaded = false;
  bookmarksWritable = false;
  currentPageBookmarked = false;
  activityManager.reportReaderOpenStage("xtc", "reader_state", readerStateStartedMs);

  // Load saved progress
  const uint32_t progressStartedMs = static_cast<uint32_t>(millis());
  loadProgress();
  if (initialBookmarkPage) {
    if (*initialBookmarkPage < xtc->getPageCount()) {
      currentPage = *initialBookmarkPage;
    } else {
      LOG_ERR("XTC", "Rejected out-of-range bookmark page: %lu", static_cast<unsigned long>(*initialBookmarkPage));
    }
    initialBookmarkPage.reset();
  }
  activityManager.reportReaderOpenStage("xtc", "progress", progressStartedMs);
  // Keep the resume target durable before rendering. Statistics and Recent
  // Books are loaded/published after the first visible page.
  const uint32_t catalogStartedMs = static_cast<uint32_t>(millis());
  if (APP_STATE.openEpubPath != xtc->getPath()) {
    APP_STATE.openEpubPath = xtc->getPath();
    readerStateSaveRetryPending = !APP_STATE.saveToFile();
    if (readerStateSaveRetryPending) LOG_ERR("XTR", "Could not persist reader resume state; retrying after first page");
  }
  activityManager.reportReaderOpenStage("xtc", "catalog_recent", catalogStartedMs);

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  pendingPageTurnDelta = 0;
  commitReadingSession();
  saveReadingStats();

  APP_STATE.readerActivityLoadCount = 0;
  if (!APP_STATE.saveToFile()) LOG_ERR("XTR", "Could not persist reader exit state");
  xtc.reset();

  sdFontSystem.releaseLoadedFont(renderer);

  if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
}

void XtcReaderActivity::onPause() {
  pendingPageTurnDelta = 0;
  clearBlockingFeedback();
  if (xtc) xtc->cancelThumbnailPreparation();
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
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  ensureBookmarksLoaded();
  updateCurrentPageBookmarked();

  uint32_t displayedPage = currentPage;
  if (lastSuccessfullyRenderedPage != std::numeric_limits<uint32_t>::max()) {
    displayedPage = lastSuccessfullyRenderedPage;
  }
  displayedPage = std::min(displayedPage, xtc->getPageCount() - 1);
  const int progressPercent =
      bookReadingStats.isCompleted
          ? 100
          : static_cast<int>((static_cast<uint64_t>(displayedPage) + 1) * 100 / xtc->getPageCount());
  // Open validation already proved the chapter table is structurally usable;
  // names stay unloaded until the TOC or an enabled chapter status bar needs them.
  const bool hasChapters = xtc->hasChapters();
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

void XtcReaderActivity::pumpDeferredCoverPreparation() {
  if (!xtc || !deferredCoverRequested || deferredCoverFinished) return;
  const uint32_t renderedPage = lastSuccessfullyRenderedPage.load(std::memory_order_acquire);
  if (renderedPage == std::numeric_limits<uint32_t>::max()) return;

  RenderLock lock(std::try_to_lock);
  if (!lock.ownsLock() || currentPage != renderedPage) return;

  const bool needsShared = CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel = CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  if (!needsShared && !needsCarousel) {
    deferredCoverFinished = true;
    return;
  }

  const bool x3 = renderer.getDisplayHeight() == 528;
  const int carouselWidth = x3 ? Epub::CAROUSEL_THUMB_WIDTH : Epub::CAROUSEL_X4_THUMB_WIDTH;
  const int carouselHeight = x3 ? Epub::CAROUSEL_THUMB_HEIGHT : Epub::CAROUSEL_X4_THUMB_HEIGHT;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (!xtc->thumbnailPreparationActive()) deferredCoverStartedMs = static_cast<uint32_t>(millis());
#endif
  const Xtc::ThumbnailPreparationStatus status = xtc->thumbnailPreparationActive()
                                                     ? xtc->stepThumbnailPreparation(1024, 8)
                                                     : xtc->beginThumbnailPreparation(carouselWidth, carouselHeight);
  if (status == Xtc::ThumbnailPreparationStatus::InProgress) return;

  deferredCoverFinished = true;
  if (status != Xtc::ThumbnailPreparationStatus::Ready) {
    LOG_ERR("XTR", "Could not prepare every requested XTC cover cache: %s", xtc->getPath().c_str());
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("ROPM", "post_visible format=xtc name=cover elapsed_ms=%u",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - deferredCoverStartedMs));
#endif
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
  bool hasChapters = false;
  {
    RenderLock lock;
    book = xtc;
    page = currentPage;
    hasChapters = book && book->hasChapters() && !book->getChapters().empty();
  }
  if (hasChapters) {
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
  const bool inputEdge = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased();
  const bool readerInputHeld = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                               mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                               mappedInput.isPressed(MappedInputManager::Button::Left) ||
                               mappedInput.isPressed(MappedInputManager::Button::Right) ||
                               mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                               mappedInput.isPressed(MappedInputManager::Button::PageForward);
  if (inputEdge) {
    deferredCoverLastInputAt = static_cast<uint32_t>(millis());
    if (xtc->thumbnailPreparationActive()) xtc->cancelThumbnailPreparation();
  }
  if (showBookmarkMessage && millis() - bookmarkMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  uint32_t pageSnapshot = lastSuccessfullyRenderedPage.load(std::memory_order_acquire);
  if (pageSnapshot == std::numeric_limits<uint32_t>::max()) pageSnapshot = 0;
  {
    RenderLock lock(std::try_to_lock);
    if (lock.ownsLock()) pageSnapshot = currentPage;
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
    if (!atEndOfBook && autoPageTurnSeconds != 0 && !inputEdge && !readerInputHeld && pendingPageTurnDelta == 0 &&
        millis() - lastPageTurnTime >= autoPageTurnInterval) {
      bool pageTurned = false;
      {
        RenderLock lock(std::try_to_lock);
        const uint32_t lockedPage = lock.ownsLock() ? currentPage : 0;
        const uint32_t pageCount = xtc->getPageCount();
        if (!lock.ownsLock() || activityManager.hasPendingRender() ||
            lastSuccessfullyRenderedPage.load(std::memory_order_acquire) != lockedPage) {
          lastPageTurnTime = millis();
        } else if (lockedPage < pageCount && millis() - lastPageTurnTime >= autoPageTurnInterval) {
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
  bool prevTriggered = pageGesture.prev;
  bool nextTriggered = pageGesture.next;
  bool drainingQueuedTurn = false;
  if (!prevTriggered && !nextTriggered && pendingPageTurnDelta != 0 && !inputEdge && !readerInputHeld &&
      !activityManager.hasPendingRender()) {
    bool forward = false;
    if (pendingPageTurnDelta < 0 && pageSnapshot == 0) {
      pendingPageTurnDelta = 0;
    } else if (ReaderUtils::takeQueuedPageTurn(pendingPageTurnDelta, forward)) {
      prevTriggered = !forward;
      nextTriggered = forward;
      drainingQueuedTurn = true;
    }
  }
  if (!prevTriggered && !nextTriggered) {
    if (!inputEdge && !readerInputHeld) {
      finishDeferredOpenState();
      if (static_cast<uint32_t>(millis()) - deferredCoverLastInputAt >= DEFERRED_COVER_IDLE_MS) {
        pumpDeferredCoverPreparation();
      }
    }
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (pageSnapshot >= xtc->getPageCount()) {
    pendingPageTurnDelta = 0;
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
    pendingPageTurnDelta = 0;
    pendingShortcutUnsupportedNotice = true;
    requestUpdate();
    return;
  }

  const int skipAmount = drainingQueuedTurn ? 1 : (pageGesture.longPress ? 10 : 1);
  const int requestedDelta = nextTriggered ? skipAmount : -skipAmount;
  if (!drainingQueuedTurn && pendingPageTurnDelta != 0) {
    ReaderUtils::queuePageTurns(pendingPageTurnDelta, requestedDelta);
    return;
  }

  RenderLock lock(std::try_to_lock);
  if (!lock.ownsLock() || activityManager.hasPendingRender() ||
      lastSuccessfullyRenderedPage.load(std::memory_order_acquire) != currentPage) {
    ReaderUtils::queuePageTurns(pendingPageTurnDelta, requestedDelta);
    lastPageTurnTime = millis();
    return;
  }

  if (prevTriggered) {
    bool changed = false;
    if (currentPage > 0) {
      consumeReadingViewSignal();
      stopReadingPage(false, static_cast<uint32_t>(millis()));
      currentPage = currentPage >= static_cast<uint32_t>(skipAmount) ? currentPage - skipAmount : 0;
      changed = true;
    }
    lock.unlock();
    if (changed) requestUpdate();
  } else if (nextTriggered) {
    bool completionFailed = false;
    consumeReadingViewSignal();
    stopReadingPage(true, static_cast<uint32_t>(millis()), !pageGesture.longPress);
    const uint32_t pageCount = xtc->getPageCount();
    const uint64_t requested = static_cast<uint64_t>(currentPage) + static_cast<uint32_t>(skipAmount);
    if (requested >= pageCount) {
      if (pageCount > 0 && lastSuccessfullyRenderedPage.load(std::memory_order_acquire) == pageCount - 1) {
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
    lock.unlock();
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

  const uint32_t renderStartedMs = readerOpenStagesPending ? static_cast<uint32_t>(millis()) : 0;
  if (renderPage(book, page)) {
    lastSuccessfullyRenderedPage = page;
    lastPageTurnTime = millis();
    if (readerOpenStagesPending) {
      activityManager.reportReaderOpenStage("xtc", "render_display", renderStartedMs);
      readerOpenStagesPending = false;
    }
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

void XtcReaderActivity::ensureBookmarksLoaded() {
  if (!bookmarksLoaded) loadBookmarks();
}

void XtcReaderActivity::loadBookmarks() {
  cachedBookmarks.clear();
  bookmarksLoaded = true;
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
  ensureBookmarksLoaded();
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
  const uint32_t visibleAtMs = static_cast<uint32_t>(millis());
  activityManager.finishReaderOpenMetric("xtc", visibleAtMs);
  pendingReadingViewAtMs.store(visibleAtMs, std::memory_order_relaxed);
  pendingReadingViewSignal.store(1, std::memory_order_release);
}

void XtcReaderActivity::signalReadingPageHidden() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(-1, std::memory_order_release);
}

void XtcReaderActivity::finishDeferredOpenState() {
  if (!deferredOpenStatePending || !deferredOpenStateReady) return;
  deferredOpenStatePending = false;

  if (readerStateSaveRetryPending) {
    readerStateSaveRetryPending = false;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("XTR", "Could not persist reader resume state after retry");
      pendingBookmarkStorageError = true;
      requestUpdate();
    }
  }

  const uint32_t statsStartedMs = static_cast<uint32_t>(millis());
  GlobalReadingStats::LoadStatus globalStatsStatus = GlobalReadingStats::LoadStatus::Missing;
  globalReadingStats = GlobalReadingStats::load(&globalStatsStatus);
  globalReadingStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(globalStatsStatus);
  globalReadingStatsWritable = completionStatsWritableAtOpen && globalReadingStatsTrusted;
  if (globalReadingStatsWritable && deferredGlobalPageTurns > 0) {
    globalReadingStats.totalPagesTurned =
        addReadingStatsSaturated(globalReadingStats.totalPagesTurned, deferredGlobalPageTurns);
    globalReadingStatsDirty = true;
  }
  deferredGlobalPageTurns = 0;
  const uint32_t recentStartedMs = static_cast<uint32_t>(millis());
  if (!skipStartupRecentUpdate) {
    RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  }
  LOG_DBG("ROPM", "post_visible format=xtc stats_ms=%u recent_ms=%u",
          static_cast<unsigned>(recentStartedMs - statsStartedMs),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - recentStartedMs));
}

void XtcReaderActivity::consumeReadingViewSignal() {
  const int8_t signal = pendingReadingViewSignal.exchange(0, std::memory_order_acq_rel);
  if (signal == 0) return;

  const uint32_t eventAtMs = pendingReadingViewAtMs.load(std::memory_order_relaxed);
  if (signal > 0) {
    deferredOpenStateReady = true;
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
  } else if (deferredOpenStatePending) {
    deferredGlobalPageTurns = addReadingStatsSaturated(deferredGlobalPageTurns, 1);
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
  finishDeferredOpenState();
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
  finishDeferredOpenState();
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
    finishDeferredOpenState();
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

  const bool nativeX4Portrait = pageWidth == renderer.getScreenWidth() && pageHeight == renderer.getScreenHeight() &&
                                renderer.getOrientation() == GfxRenderer::Portrait &&
                                renderer.getDisplayWidth() == pageHeight && renderer.getDisplayHeight() == pageWidth;

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t renderStartedMs = static_cast<uint32_t>(millis());
  const uint32_t renderStartFreeHeap = ESP.getFreeHeap();
#endif

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

    // XTCH is two roughly 48 KiB bit planes. In portrait, matching source
    // columns map to physical framebuffer rows, so X4 and scaled X3 can consume
    // paired chunks without materializing a full source plane.
    renderer.clearScreen();
    const bool nativeX4Xtch = nativeX4Portrait && renderer.hasFrameBuffer() &&
                              renderer.getBufferSize() >= pageLayout.planeBytes &&
                              renderer.getDisplayWidthBytes() == pageLayout.columnBytes;
    constexpr size_t XTH_PAIR_CHUNK_BYTES = 1000;
    const size_t pairedColumns = pageLayout.columnBytes == 0 ? 0 : XTH_PAIR_CHUNK_BYTES / pageLayout.columnBytes;
    const size_t panelRowBytes = renderer.getDisplayWidthBytes();
    const bool portraitInverted = renderer.getOrientation() == GfxRenderer::PortraitInverted;
    size_t pairedPlaneScratchBytes = 0;
    size_t pairedScratchBytes = 0;
    const bool scaledPairCandidate =
        !nativeX4Xtch && (renderer.getOrientation() == GfxRenderer::Portrait || portraitInverted) &&
        renderer.hasFrameBuffer() && renderer.supportsStripGrayscale() && viewport.width <= pageWidth &&
        viewport.height <= pageHeight && pairedColumns > 0 && renderer.getDisplayWidth() % 8U == 0 &&
        renderer.getBufferSize() >= static_cast<size_t>(renderer.getDisplayHeight()) * panelRowBytes &&
        xtc::checkedMultiply(pairedColumns, panelRowBytes, pairedPlaneScratchBytes) &&
        xtc::checkedMultiply(pairedPlaneScratchBytes, 2U, pairedScratchBytes);
    if (scaledPairCandidate && xtchPlaneScratchCapacity < pairedScratchBytes) {
      auto replacement = makeUniqueNoThrow<uint8_t[]>(pairedScratchBytes);
      if (replacement) {
        xtchPlaneScratch = std::move(replacement);
        xtchPlaneScratchCapacity = pairedScratchBytes;
      }
    }
    const bool scaledPairedXtch =
        scaledPairCandidate && xtchPlaneScratch && xtchPlaneScratchCapacity >= pairedScratchBytes;
    xtc::XtcError streamError = xtc::XtcError::OK;
    if (nativeX4Xtch) {
      uint8_t* const frameBuffer = renderer.getFrameBuffer();
      streamError =
          book->loadXthPlanePairs(page, [&](uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t planeOffset) {
            xtc::composeNativeXthPlaneBytes(bit0, bit1, size, bit0, nullptr, nullptr);
            std::memcpy(frameBuffer + planeOffset, bit0, size);
          });
    } else if (scaledPairedXtch) {
      bool composed = true;
      uint8_t* const frameBuffer = renderer.getFrameBuffer();
      streamError = book->loadXthPlanePairs(
          page,
          [&](uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t planeOffset) {
            if (!composed) return;
            xtc::XthPortraitRows rows;
            composed = xtc::composeScaledXthPortraitRows(
                bit0, bit1, size, planeOffset, pageLayout, pageWidth, pageHeight, viewport, renderer.getDisplayWidth(),
                renderer.getDisplayHeight(), portraitInverted, xtchPlaneScratch.get(), nullptr, nullptr,
                pairedPlaneScratchBytes, rows);
            const size_t rowOffset = static_cast<size_t>(rows.yStart) * panelRowBytes;
            const size_t rowBytes = static_cast<size_t>(rows.count) * panelRowBytes;
            if (composed && rows.count > 0 && rowOffset <= renderer.getBufferSize() &&
                rowBytes <= renderer.getBufferSize() - rowOffset) {
              std::memcpy(frameBuffer + rowOffset, xtchPlaneScratch.get(), rowBytes);
            } else if (rows.count > 0) {
              composed = false;
            }
          },
          XTH_PAIR_CHUNK_BYTES);
      if (streamError == xtc::XtcError::OK && !composed) streamError = xtc::XtcError::SIZE_MISMATCH;
    } else {
      streamError = streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer,
                                         XtchRenderPass::Base);
    }
    if (streamError != xtc::XtcError::OK) {
      showStreamError(streamError);
      return false;
    }
    renderStatusBar();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t baseRenderedMs = static_cast<uint32_t>(millis());
#endif

    if (!renderer.supportsStripGrayscale()) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      const uint32_t renderFinishedMs = static_cast<uint32_t>(millis());
      const uint32_t renderFreeHeap = ESP.getFreeHeap();
      LOG_DBG("XTRM",
              "page=%lu depth=2 fallback=1 base_ms=%u display_ms=%u total_ms=%u heap_delta=%ld "
              "free_heap=%u max_alloc=%u",
              static_cast<unsigned long>(page), static_cast<unsigned>(baseRenderedMs - renderStartedMs),
              static_cast<unsigned>(renderFinishedMs - baseRenderedMs),
              static_cast<unsigned>(renderFinishedMs - renderStartedMs),
              static_cast<long>(static_cast<int32_t>(renderStartFreeHeap) - static_cast<int32_t>(renderFreeHeap)),
              static_cast<unsigned>(renderFreeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
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
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t baseDisplayedMs = static_cast<uint32_t>(millis());
#endif

    if (nativeX4Xtch) {
      streamError =
          book->loadXthPlanePairs(page, [&](uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t planeOffset) {
            xtc::composeNativeXthPlaneBytes(bit0, bit1, size, nullptr, bit0, bit1);
            const int yStart = static_cast<int>(planeOffset / pageLayout.columnBytes);
            const int rows = static_cast<int>(size / pageLayout.columnBytes);
            renderer.writeGrayscalePlaneStrip(true, bit0, yStart, rows);
            renderer.writeGrayscalePlaneStrip(false, bit1, yStart, rows);
          });
      if (streamError != xtc::XtcError::OK) {
        showStreamError(streamError);
        return false;
      }
    } else if (scaledPairedXtch) {
      uint8_t* const lsbScratch = xtchPlaneScratch.get();
      uint8_t* const msbScratch = lsbScratch + pairedPlaneScratchBytes;
      std::memset(lsbScratch, 0, pairedPlaneScratchBytes);
      const auto clearRows = [&](const bool lsbPlane, uint16_t yStart, uint16_t rows) {
        while (rows > 0) {
          const uint16_t chunkRows = static_cast<uint16_t>(std::min<size_t>(rows, pairedColumns));
          renderer.writeGrayscalePlaneStrip(lsbPlane, lsbScratch, yStart, chunkRows);
          yStart = static_cast<uint16_t>(yStart + chunkRows);
          rows = static_cast<uint16_t>(rows - chunkRows);
        }
      };
      const uint16_t activeRowStart =
          portraitInverted ? viewport.x
                           : static_cast<uint16_t>(renderer.getDisplayHeight() - viewport.x - viewport.width);
      const uint16_t activeRowEnd = portraitInverted ? static_cast<uint16_t>(viewport.x + viewport.width)
                                                     : static_cast<uint16_t>(renderer.getDisplayHeight() - viewport.x);
      clearRows(true, 0, activeRowStart);
      clearRows(false, 0, activeRowStart);
      clearRows(true, activeRowEnd, static_cast<uint16_t>(renderer.getDisplayHeight() - activeRowEnd));
      clearRows(false, activeRowEnd, static_cast<uint16_t>(renderer.getDisplayHeight() - activeRowEnd));

      bool composed = true;
      streamError = book->loadXthPlanePairs(
          page,
          [&](uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t planeOffset) {
            if (!composed) return;
            xtc::XthPortraitRows rows;
            composed = xtc::composeScaledXthPortraitRows(bit0, bit1, size, planeOffset, pageLayout, pageWidth,
                                                         pageHeight, viewport, renderer.getDisplayWidth(),
                                                         renderer.getDisplayHeight(), portraitInverted, nullptr,
                                                         lsbScratch, msbScratch, pairedPlaneScratchBytes, rows);
            if (composed && rows.count > 0) {
              renderer.writeGrayscalePlaneStrip(true, lsbScratch, rows.yStart, rows.count);
              renderer.writeGrayscalePlaneStrip(false, msbScratch, rows.yStart, rows.count);
            }
          },
          XTH_PAIR_CHUNK_BYTES);
      if (streamError == xtc::XtcError::OK && !composed) streamError = xtc::XtcError::SIZE_MISMATCH;
      if (streamError != xtc::XtcError::OK) {
        showStreamError(streamError);
        return false;
      }
    } else {
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
    }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t planesRenderedMs = static_cast<uint32_t>(millis());
#endif
    renderer.displayGrayBuffer();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t grayscaleDisplayedMs = static_cast<uint32_t>(millis());
#endif

    if (!nativeX4Xtch && !scaledPairedXtch) {
      renderer.clearScreen();
      streamError = streamXtchRenderPass(*book, page, pageLayout, pageWidth, pageHeight, viewport, renderer,
                                         XtchRenderPass::Base);
      if (streamError != xtc::XtcError::OK) {
        showStreamError(streamError);
        return false;
      }
      renderStatusBar();
    }
    renderer.cleanupGrayscaleWithFrameBuffer();

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t renderFinishedMs = static_cast<uint32_t>(millis());
    const uint32_t renderFreeHeap = ESP.getFreeHeap();
    LOG_DBG("XTRM",
            "page=%lu depth=2 native_x4=%d scaled_pair=%d read_passes=%u base_ms=%u base_display_ms=%u planes_ms=%u "
            "gray_display_ms=%u cleanup_ms=%u total_ms=%u heap_delta=%ld free_heap=%u max_alloc=%u",
            static_cast<unsigned long>(page), nativeX4Xtch ? 1 : 0, scaledPairedXtch ? 1 : 0,
            nativeX4Xtch || scaledPairedXtch ? 2U : 4U, static_cast<unsigned>(baseRenderedMs - renderStartedMs),
            static_cast<unsigned>(baseDisplayedMs - baseRenderedMs),
            static_cast<unsigned>(planesRenderedMs - baseDisplayedMs),
            static_cast<unsigned>(grayscaleDisplayedMs - planesRenderedMs),
            static_cast<unsigned>(renderFinishedMs - grayscaleDisplayedMs),
            static_cast<unsigned>(renderFinishedMs - renderStartedMs),
            static_cast<long>(static_cast<int32_t>(renderStartFreeHeap) - static_cast<int32_t>(renderFreeHeap)),
            static_cast<unsigned>(renderFreeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", page + 1, book->getPageCount());
    return true;
  }

  if (nativeX4Portrait) {
    renderer.clearScreen();
    bool rotated = true;
    const xtc::XtcError streamError = book->loadPageStreaming(
        page,
        [&](const uint8_t* data, const size_t size, const size_t offset) {
          if (rotated) {
            rotated = xtc::rotateXtgPortraitRowsToNativeLandscape(data, size, offset, pageWidth, pageHeight,
                                                                  renderer.getFrameBuffer(), renderer.getBufferSize());
          }
        },
        pageLayout.rowBytes * 8U);
    if (streamError != xtc::XtcError::OK || !rotated) {
      LOG_ERR("XTR", "Failed to stream native XTC page %lu: %s", page, xtc::errorToString(streamError));
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return false;
    }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t pageLoadedMs = static_cast<uint32_t>(millis());
#endif
    renderStatusBar();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t pageRasterizedMs = static_cast<uint32_t>(millis());
#endif
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t renderFinishedMs = static_cast<uint32_t>(millis());
    const uint32_t renderFreeHeap = ESP.getFreeHeap();
    LOG_DBG("XTRM",
            "page=%lu depth=1 native_x4=1 stream_rotate_ms=%u overlay_ms=%u display_ms=%u total_ms=%u "
            "heap_delta=%ld free_heap=%u max_alloc=%u",
            static_cast<unsigned long>(page), static_cast<unsigned>(pageLoadedMs - renderStartedMs),
            static_cast<unsigned>(pageRasterizedMs - pageLoadedMs),
            static_cast<unsigned>(renderFinishedMs - pageRasterizedMs),
            static_cast<unsigned>(renderFinishedMs - renderStartedMs),
            static_cast<long>(static_cast<int32_t>(renderStartFreeHeap) - static_cast<int32_t>(renderFreeHeap)),
            static_cast<unsigned>(renderFreeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
    LOG_DBG("XTR", "Rendered page %lu/%lu (1-bit)", page + 1, book->getPageCount());
    return true;
  }

  std::array<uint16_t, xtc::DISPLAY_WIDTH> sourceXByDestination{};
  if (viewport.width > sourceXByDestination.size()) return false;
  for (uint16_t destinationX = 0; destinationX < viewport.width; ++destinationX) {
    sourceXByDestination[destinationX] = xtc::mapViewportCoordinate(destinationX, viewport.width, pageWidth);
  }

  bool rasterValid = true;
  renderer.clearScreen();
  const size_t sourceRowBytes = pageLayout.rowBytes;
  const xtc::XtcError streamError = book->loadPageStreaming(
      page,
      [&](const uint8_t* data, const size_t size, const size_t offset) {
        if (!rasterValid || sourceRowBytes == 0 || offset % sourceRowBytes != 0 || size % sourceRowBytes != 0) {
          rasterValid = false;
          return;
        }
        const size_t firstSourceRow = offset / sourceRowBytes;
        const size_t sourceRowCount = size / sourceRowBytes;
        if (firstSourceRow > pageHeight || sourceRowCount > pageHeight - firstSourceRow) {
          rasterValid = false;
          return;
        }

        for (size_t localRow = 0; localRow < sourceRowCount; ++localRow) {
          const uint16_t sourceY = static_cast<uint16_t>(firstSourceRow + localRow);
          const xtc::CoordinateRange destinationY = xtc::mapSourceCoordinateRange(sourceY, pageHeight, viewport.height);
          const uint8_t* sourceRow = data + localRow * sourceRowBytes;
          for (uint16_t y = destinationY.begin; y < destinationY.end; ++y) {
            for (uint16_t destinationX = 0; destinationX < viewport.width; ++destinationX) {
              const uint16_t sourceX = sourceXByDestination[destinationX];
              const uint8_t sourceByte = sourceRow[sourceX / 8U];
              const uint8_t sourceBit = static_cast<uint8_t>(7U - sourceX % 8U);
              if (((sourceByte >> sourceBit) & 1U) == 0) {
                renderer.drawPixel(viewport.x + destinationX, viewport.y + y, true);
              }
            }
          }
        }
      },
      sourceRowBytes * 8U);
  if (streamError != xtc::XtcError::OK || !rasterValid) {
    LOG_ERR("XTR", "Failed to stream XTC page %lu: error=%s raster_valid=%d", page, xtc::errorToString(streamError),
            rasterValid ? 1 : 0);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t pageStreamedMs = static_cast<uint32_t>(millis());
#endif
  renderStatusBar();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t pageRasterizedMs = static_cast<uint32_t>(millis());
#endif
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t renderFinishedMs = static_cast<uint32_t>(millis());
  const uint32_t renderFreeHeap = ESP.getFreeHeap();
  LOG_DBG("XTRM",
          "page=%lu depth=%u stream_raster_ms=%u overlay_ms=%u display_ms=%u total_ms=%u heap_delta=%ld "
          "free_heap=%u max_alloc=%u",
          static_cast<unsigned long>(page), static_cast<unsigned>(bitDepth),
          static_cast<unsigned>(pageStreamedMs - renderStartedMs),
          static_cast<unsigned>(pageRasterizedMs - pageStreamedMs),
          static_cast<unsigned>(renderFinishedMs - pageRasterizedMs),
          static_cast<unsigned>(renderFinishedMs - renderStartedMs),
          static_cast<long>(static_cast<int32_t>(renderStartFreeHeap) - static_cast<int32_t>(renderFreeHeap)),
          static_cast<unsigned>(renderFreeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", page + 1, book->getPageCount(), bitDepth);
  return true;
}

bool XtcReaderActivity::saveProgress(const std::shared_ptr<Xtc>& book, const uint32_t page) {
  uint8_t data[4];
  ProgressFileCodec::encodePage(page, data);
  const ProgressFile::PageBounds bounds{book->getPageCount()};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t startedMs = static_cast<uint32_t>(millis());
#endif
  const bool saved = progressWriteSession.writeAtomic(book->getCachePath(), data, sizeof(data), validator);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("XTRM", "progress_save_ms=%u ok=%d", static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedMs),
          saved ? 1 : 0);
#endif
  if (!saved) {
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
