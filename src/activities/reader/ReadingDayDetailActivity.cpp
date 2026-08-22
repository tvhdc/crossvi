#include "ReadingDayDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
std::string formatDuration(const uint32_t seconds) {
  char value[24];
  if (seconds == 0) {
    snprintf(value, sizeof(value), "0m");
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "<1m");
  } else if (seconds < 3600) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  }
  return value;
}

std::string titleFor(const DailyBookReadingRecord& book) {
  if (!book.title.empty()) return book.title;
  const size_t slash = book.path.find_last_of('/');
  return slash == std::string::npos ? book.path : book.path.substr(slash + 1);
}
}  // namespace

void ReadingDayDetailActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  historyRekeyPending_ = !DailyBookReadingHistory::recoverPreparedRekey();
  if (cell_.date.isValid()) {
    bookHistoryStatus_ = DailyBookReadingHistory::load(readingStatsDayIndex(cell_.date), books_);
  }
  selected_ = books_.count == 0 ? 0 : std::min(selected_, books_.count - 1);
  requestUpdate();
}

void ReadingDayDetailActivity::move(const int delta) {
  if (books_.count == 0 || delta == 0) return;
  const size_t previous = selected_;
  selected_ = delta < 0 ? (selected_ == 0 ? books_.count - 1 : selected_ - 1) : (selected_ + 1) % books_.count;
  if (selected_ != previous) requestUpdate();
}

void ReadingDayDetailActivity::loop() {
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
}

void ReadingDayDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  char date[16] = "--/--/----";
  if (cell_.date.isValid()) {
    snprintf(date, sizeof(date), "%02u/%02u/%04u", static_cast<unsigned>(cell_.date.day),
             static_cast<unsigned>(cell_.date.month), static_cast<unsigned>(cell_.date.year));
  }
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_STATS_CALENDAR));
  const std::string duration = cell_.exactDuration ? formatDuration(cell_.readingSeconds) : "--";
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, date, duration.c_str());

  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  const Rect content{safe.x, contentTop, safe.width, std::max(1, contentBottom - contentTop)};
  if (books_.count > 0) {
    GUI.drawList(
        renderer, content, books_.count, static_cast<int>(selected_),
        [this](const int index) { return titleFor(books_.records[static_cast<size_t>(index)]); },
        [this](const int index) { return formatDuration(books_.records[static_cast<size_t>(index)].seconds); },
        [this](const int index) { return UITheme::getFileIcon(books_.records[static_cast<size_t>(index)].path); });
  } else {
    const bool unavailable = historyRekeyPending_ || cell_.readingSeconds > 0 ||
                             bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::Invalid ||
                             bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::NewerVersion ||
                             bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::IoError;
    GUI.drawList(renderer, content, 1, -1, [unavailable](int) {
      const StrId message = unavailable ? StrId::STR_STATS_BOOK_DETAILS_UNAVAILABLE : StrId::STR_STATS_NO_DATA;
      return std::string(I18n::getInstance().get(message));
    });
  }

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
