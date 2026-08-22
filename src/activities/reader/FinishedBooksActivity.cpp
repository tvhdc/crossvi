#include "FinishedBooksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include "BookStatsLoader.h"
#include "FinishedBooksStore.h"
#include "MappedInputManager.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsPresentation.h"
#include "ReadingStatsUtils.h"
#include "RecentBooksStore.h"
#include "components/LibraryGridModel.h"
#include "components/UITheme.h"

namespace {
std::string titleFor(const FinishedBook& book) {
  if (!book.title.empty()) return book.title;
  const size_t slash = book.path.find_last_of('/');
  return slash == std::string::npos ? book.path : book.path.substr(slash + 1);
}

std::string subtitleFor(const FinishedBook& book) {
  std::string value;
  ReadingStatsDate date;
  if (book.finishedDay != 0 && readingStatsDateFromDayIndex(book.finishedDay, date)) {
    char formatted[16]{};
    std::snprintf(formatted, sizeof(formatted), "%02u/%02u/%04u", static_cast<unsigned>(date.day),
                  static_cast<unsigned>(date.month), static_cast<unsigned>(date.year));
    value = formatted;
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
  statsLoadFailed_ = false;
  FINISHED_BOOKS.pruneMissing();
  const size_t count = FINISHED_BOOKS.books().size();
  selected_ = count == 0 ? 0 : std::min(selected_, count - 1);
  requestUpdate();
}

void FinishedBooksActivity::openSelectedStatistics() {
  const auto& books = FINISHED_BOOKS.books();
  if (selected_ >= books.size()) return;
  const FinishedBook& selected = books[selected_];
  const RecentBook recent{selected.path, selected.title, selected.author, {}};
  ReadingStatsPresentation presentation;
  if (!loadBookStatsPresentation(recent, presentation)) {
    statsLoadFailed_ = true;
    requestUpdate();
    return;
  }
  statsLoadFailed_ = false;
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, titleFor(selected), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, false, false, selected.path),
      [this](const ActivityResult&) { requestUpdate(); });
}

void FinishedBooksActivity::move(const int delta) {
  const size_t count = FINISHED_BOOKS.books().size();
  if (count == 0) return;
  selected_ =
      delta < 0 ? LibraryGridModel::previousIndex(selected_, count) : LibraryGridModel::nextIndex(selected_, count);
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
  const auto& books = FINISHED_BOOKS.books();
  const bool empty = books.empty();
  if (statsLoadFailed_) {
    GUI.drawList(renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentHeight)}, 1, -1,
                 [](int) { return std::string(tr(STR_STATS_UNAVAILABLE)); });
  } else {
    GUI.drawList(
        renderer, Rect{safe.x, contentTop, safe.width, std::max(1, contentHeight)}, empty ? 1 : books.size(),
        empty ? -1 : static_cast<int>(selected_),
        [&books, empty](const int index) {
          return empty ? std::string(tr(STR_STATS_NO_DATA)) : titleFor(books[static_cast<size_t>(index)]);
        },
        [&books, empty](const int index) {
          return empty ? std::string{} : subtitleFor(books[static_cast<size_t>(index)]);
        },
        [&books, empty](const int index) {
          return empty ? UIIcon::None : UITheme::getFileIcon(books[static_cast<size_t>(index)].path);
        });
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
