#include "ReadingDayDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "ClockDateFormat.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingCalendarRenderer.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
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
    const uint32_t day = readingStatsDayIndex(cell_.date);
    bookHistoryStatus_ = DailyBookReadingHistory::load(day, books_);
    uint32_t recordedSeconds = 0;
    for (size_t index = 0; index < books_.count; ++index) {
      recordedSeconds = addReadingStatsSaturated(recordedSeconds, books_.records[index].seconds);
    }

    DailyReadingHistory canonicalHistory;
    const DailyReadingHistory::LoadStatus canonicalStatus = DailyReadingHistory::load(canonicalHistory);
    uint32_t canonicalSeconds = 0;
    const bool canonicalTrusted = canonicalStatus == DailyReadingHistory::LoadStatus::Ok ||
                                  canonicalStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                                  canonicalStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
    const bool canonicalDayKnown = canonicalTrusted && canonicalHistory.valueForDay(day, canonicalSeconds);
    const bool recovered = bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::RecoveredBackup ||
                           bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::RecoveredTemp ||
                           canonicalStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                           canonicalStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
    const bool bookHistoryUnreadable = bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::Invalid ||
                                       bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::NewerVersion ||
                                       bookHistoryStatus_ == DailyBookReadingHistory::LoadStatus::IoError;
    const bool canonicalUnreadable = canonicalStatus == DailyReadingHistory::LoadStatus::Invalid ||
                                     canonicalStatus == DailyReadingHistory::LoadStatus::NewerVersion ||
                                     canonicalStatus == DailyReadingHistory::LoadStatus::IoError;
    breakdownPartial_ = historyRekeyPending_ || recovered || bookHistoryUnreadable || canonicalUnreadable ||
                        (canonicalDayKnown ? recordedSeconds != canonicalSeconds : cell_.readingSeconds > 0);
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
    ClockDateFormat::format(cell_.date.year, cell_.date.month, cell_.date.day, SETTINGS.dateFormat,
                           ClockDateFormat::separatorChar(SETTINGS.dateSeparator), date, sizeof(date),
                           I18N.getLanguage() == Language::VI);
  }
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_STATS_CALENDAR));
  const std::string duration =
      cell_.exactDuration ? ReadingCalendarRenderer::formatDuration(cell_.readingSeconds) : "--";
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, date, duration.c_str());

  int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  if (breakdownPartial_ && books_.count > 0) {
    const char* message = tr(STR_STATS_BOOK_DETAILS_PARTIAL);
    const std::string display =
        renderer.truncatedText(UI_10_FONT_ID, message, safe.width - metrics.contentSidePadding * 2);
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding, contentTop, display.c_str());
    contentTop += renderer.getTextHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  }
  const Rect content{safe.x, contentTop, safe.width, std::max(1, contentBottom - contentTop)};
  if (books_.count > 0) {
    GUI.drawList(
        renderer, content, books_.count, static_cast<int>(selected_),
        [this](const int index) { return titleFor(books_.records[static_cast<size_t>(index)]); },
        [this](const int index) {
          return ReadingCalendarRenderer::formatDuration(books_.records[static_cast<size_t>(index)].seconds);
        },
        [this](const int index) { return UITheme::getFileIcon(books_.records[static_cast<size_t>(index)].path); });
  } else {
    const bool unavailable = breakdownPartial_ || cell_.readingSeconds > 0;
    GUI.drawList(renderer, content, 1, -1, [unavailable](int) {
      const StrId message = unavailable ? StrId::STR_STATS_BOOK_DETAILS_UNAVAILABLE : StrId::STR_STATS_NO_DATA;
      return std::string(I18n::getInstance().get(message));
    });
  }

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
