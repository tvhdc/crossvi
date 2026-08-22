#include "BookReadingHistoryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "DailyBookReadingHistory.h"
#include "MappedInputManager.h"
#include "ReadingStatsUtils.h"
#include "components/UITheme.h"

namespace {
std::string formatDuration(const uint32_t seconds) {
  char value[24];
  if (seconds < 60) {
    snprintf(value, sizeof(value), seconds == 0 ? "0m" : "<1m");
  } else if (seconds < 3600) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  }
  return value;
}

std::string formatDate(const uint32_t day) {
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date)) return "--/--/----";
  char value[16];
  snprintf(value, sizeof(value), "%02u/%02u/%04u", static_cast<unsigned>(date.day), static_cast<unsigned>(date.month),
           static_cast<unsigned>(date.year));
  return value;
}
}  // namespace

void BookReadingHistoryActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!DailyBookReadingHistory::recoverPreparedRekey()) {
    scanPartial_ = true;
    DailyBookReadingHistory::pendingRekeyAlias(bookPath_, pendingAlias_);
  }
  if (bookPath_.empty() || !Storage.exists(DailyBookReadingHistory::DIRECTORY)) {
    scanComplete_ = true;
  } else {
    directory_ = Storage.open(DailyBookReadingHistory::DIRECTORY);
    if (!directory_ || !directory_.isDirectory()) finishScan(true);
  }
  requestUpdate();
}

void BookReadingHistoryActivity::onExit() {
  if (directory_ && !directory_.close()) LOG_ERR("BKH", "Could not close daily history directory");
  Activity::onExit();
}

void BookReadingHistoryActivity::addEntry(const uint32_t day, const uint32_t seconds) {
  if (seconds == 0) return;
  if (entryCount_ < entries_.size()) {
    entries_[entryCount_++] = {day, seconds};
    return;
  }
  const auto oldest =
      std::min_element(entries_.begin(), entries_.end(),
                       [](const HistoryEntry& lhs, const HistoryEntry& rhs) { return lhs.day < rhs.day; });
  if (oldest != entries_.end() && day > oldest->day) *oldest = {day, seconds};
}

void BookReadingHistoryActivity::finishScan(const bool failed) {
  scanPartial_ = scanPartial_ || failed;
  if (directory_ && !directory_.close()) scanPartial_ = true;
  std::sort(entries_.begin(), entries_.begin() + entryCount_,
            [](const HistoryEntry& lhs, const HistoryEntry& rhs) { return lhs.day > rhs.day; });
  selected_ = entryCount_ == 0 ? 0 : std::min(selected_, entryCount_ - 1);
  scanComplete_ = true;
  requestUpdate();
}

void BookReadingHistoryActivity::stepScan() {
  if (scanComplete_ || !directory_) return;
  HalFile file = directory_.openNextFile();
  if (!file) {
    finishScan(directory_.getError() != 0);
    return;
  }

  char name[32]{};
  const bool isDirectory = file.isDirectory();
  const size_t nameLength = file.getName(name, sizeof(name));
  const bool entryFailed = file.getError() != 0 || !file.close();
  if (entryFailed) {
    scanPartial_ = true;
    return;
  }
  uint32_t day = 0;
  if (isDirectory || nameLength == 0 || nameLength >= sizeof(name) ||
      !DailyBookReadingHistory::dayFromFileName(name, day)) {
    return;
  }

  DailyBookReadingDay daily;
  const DailyBookReadingHistory::LoadStatus status = DailyBookReadingHistory::load(day, daily);
  if (status != DailyBookReadingHistory::LoadStatus::Ok &&
      status != DailyBookReadingHistory::LoadStatus::RecoveredBackup &&
      status != DailyBookReadingHistory::LoadStatus::RecoveredTemp) {
    scanPartial_ = scanPartial_ || status != DailyBookReadingHistory::LoadStatus::Missing;
    return;
  }
  uint32_t seconds = 0;
  for (size_t index = 0; index < daily.count; ++index) {
    const DailyBookReadingRecord& record = daily.records[index];
    if (record.path == bookPath_ || (!pendingAlias_.empty() && record.path == pendingAlias_)) {
      seconds = addReadingStatsSaturated(seconds, record.seconds);
    }
  }
  addEntry(day, seconds);
}

void BookReadingHistoryActivity::move(const int delta) {
  if (!scanComplete_ || entryCount_ == 0 || delta == 0) return;
  const size_t next = delta < 0 ? ButtonNavigator::previousIndex(selected_, entryCount_)
                                : ButtonNavigator::nextIndex(selected_, entryCount_);
  if (next == selected_) return;
  selected_ = next;
  requestUpdate();
}

void BookReadingHistoryActivity::loop() {
  if (suppressInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease_ = false;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
  navigator_.onPrevious([this] { move(-1); });
  navigator_.onNext([this] { move(1); });
  if (!scanComplete_) stepScan();
}

void BookReadingHistoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_READING_STATS),
                 bookTitle_.c_str());
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight},
                    tr(STR_STATS_READING_HISTORY), scanPartial_ ? tr(STR_STATS_DATED_DATA_PARTIAL) : nullptr);

  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  const Rect content{safe.x, contentTop, safe.width, std::max(1, contentBottom - contentTop)};
  if (!scanComplete_) {
    GUI.drawList(renderer, content, 1, -1, [](int) { return std::string(tr(STR_LOADING)); });
  } else if (entryCount_ == 0) {
    GUI.drawList(renderer, content, 1, -1, [](int) { return std::string(tr(STR_STATS_BOOK_HISTORY_UNAVAILABLE)); });
  } else {
    GUI.drawList(
        renderer, content, entryCount_, static_cast<int>(selected_),
        [this](const int index) { return formatDate(entries_[static_cast<size_t>(index)].day); },
        [this](const int index) { return formatDuration(entries_[static_cast<size_t>(index)].seconds); },
        [](int) { return UIIcon::Recent; });
  }

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_STATS_PREVIOUS), tr(STR_STATS_NEXT));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
