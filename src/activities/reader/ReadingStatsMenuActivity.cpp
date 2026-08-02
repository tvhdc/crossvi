#include "ReadingStatsMenuActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <memory>

#include "BookStatsSelectionActivity.h"
#include "GlobalReadingStats.h"
#include "MappedInputManager.h"
#include "ReadingCalendarActivity.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsPresentation.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace {
constexpr std::array<StrId, 3> TITLES = {StrId::STR_STATS_OVERVIEW, StrId::STR_STATS_BY_BOOK,
                                         StrId::STR_STATS_CALENDAR};
constexpr std::array<StrId, 3> SUBTITLES = {StrId::STR_STATS_OVERVIEW_SUBTITLE, StrId::STR_STATS_BY_BOOK_SUBTITLE,
                                            StrId::STR_STATS_CALENDAR_SUBTITLE};
constexpr std::array<UIIcon, 3> ICONS = {UIIcon::Book, UIIcon::Library, UIIcon::Recent};

bool loadDevicePresentation(ReadingStatsPresentation& presentation) {
  if (!Storage.probeMedia()) return false;
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats stats = GlobalReadingStats::load(&status);
  if (!GlobalReadingStats::isTrustedLoadStatus(status)) return false;
  const GlobalReadingStatsAggregation aggregate = GlobalReadingStats::hasSyncedStats()
                                                      ? GlobalReadingStats::loadAggregatedWithReport(stats)
                                                      : GlobalReadingStatsAggregation{};
  ReadingStatsDateTime now;
  const ReadingStatsDateTime* current = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  presentation = buildReadingStatsPresentation({}, true, stats, true, aggregate, current,
                                               ReadingStatsMetric::notApplicable(), false);
  return true;
}
}  // namespace

void ReadingStatsMenuActivity::onEnter() {
  Activity::onEnter();
  suppressInitialConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingStatsMenuActivity::setNotice(const Notice notice) {
  {
    RenderLock lock(*this);
    notice_ = notice;
  }
  requestUpdate();
}

void ReadingStatsMenuActivity::openOverview() {
  ReadingStatsPresentation presentation;
  if (!loadDevicePresentation(presentation)) {
    setNotice(Notice::Unavailable);
    return;
  }
  setNotice(Notice::None);
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, std::string{}, std::move(presentation),
                                             ReadingStatsActivity::Page::Device, false, true),
      [this](const ActivityResult& result) { handleStatsAction(result); });
}

void ReadingStatsMenuActivity::openCalendar() {
  ReadingStatsPresentation presentation;
  if (!loadDevicePresentation(presentation)) {
    setNotice(Notice::Unavailable);
    return;
  }
  setNotice(Notice::None);
  startActivityForResult(std::make_unique<ReadingCalendarActivity>(renderer, mappedInput, presentation.deviceCalendar),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void ReadingStatsMenuActivity::handleStatsAction(const ActivityResult& result) {
  const auto* action = std::get_if<ReadingStatsActionResult>(&result.data);
  if (!action) {
    setNotice(Notice::None);
    return;
  }
  if (action->action == ReadingStatsActionResult::Action::BackupDeviceStats) {
    setNotice(GlobalReadingStats::createBackup() == GlobalReadingStats::BackupResult::Ok ? Notice::BackupDone
                                                                                         : Notice::BackupFailed);
    return;
  }
  if (action->action != ReadingStatsActionResult::Action::RestoreDeviceStats) return;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_STATS_RESTORE),
                                                                tr(STR_STATS_RESTORE_PROMPT)),
                         [this](const ActivityResult& confirmation) {
                           if (confirmation.isCancelled) {
                             setNotice(Notice::None);
                             return;
                           }
                           const GlobalReadingStats::BackupResult restored = GlobalReadingStats::restoreBackup();
                           if (restored == GlobalReadingStats::BackupResult::Ok) {
                             setNotice(Notice::RestoreDone);
                           } else if (restored == GlobalReadingStats::BackupResult::Missing) {
                             setNotice(Notice::NoBackup);
                           } else {
                             setNotice(Notice::RestoreFailed);
                           }
                         });
}

void ReadingStatsMenuActivity::openSelected() {
  if (!Storage.probeMedia()) {
    setNotice(Notice::Unavailable);
    return;
  }
  int selected = 0;
  {
    RenderLock lock(*this);
    selected = selectedIndex_;
  }
  switch (selected) {
    case 0:
      openOverview();
      break;
    case 1:
      setNotice(Notice::None);
      startActivityForResult(std::make_unique<BookStatsSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { setNotice(Notice::None); });
      break;
    case 2:
      openCalendar();
      break;
    default:
      break;
  }
}

void ReadingStatsMenuActivity::loop() {
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
  navigator_.onNextPress([this] {
    bool changed = false;
    {
      RenderLock lock(*this);
      const int next = ButtonNavigator::nextIndex(selectedIndex_, static_cast<int>(TITLES.size()));
      if (next != selectedIndex_) {
        selectedIndex_ = next;
        notice_ = Notice::None;
        changed = true;
      }
    }
    if (changed) requestUpdate();
  });
  navigator_.onPreviousPress([this] {
    bool changed = false;
    {
      RenderLock lock(*this);
      const int previous = ButtonNavigator::previousIndex(selectedIndex_, static_cast<int>(TITLES.size()));
      if (previous != selectedIndex_) {
        selectedIndex_ = previous;
        notice_ = Notice::None;
        changed = true;
      }
    }
    if (changed) requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelected();
}

void ReadingStatsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_READING_STATS));
  const int contentTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int listHeight = std::min(static_cast<int>(TITLES.size()) * rowHeight, contentBottom - contentTop);
  GUI.drawList(
      renderer, Rect{safe.x, contentTop, safe.width, listHeight}, TITLES.size(), selectedIndex_,
      [](const int index) { return std::string(I18N.get(TITLES[static_cast<size_t>(index)])); },
      [](const int index) { return std::string(I18N.get(SUBTITLES[static_cast<size_t>(index)])); },
      [](const int index) { return ICONS[static_cast<size_t>(index)]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_STATS_PREVIOUS), tr(STR_STATS_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (notice_ != Notice::None) {
    StrId message = StrId::STR_STATS_BACKUP_FAILED;
    switch (notice_) {
      case Notice::Unavailable:
        message = StrId::STR_STATS_UNAVAILABLE;
        break;
      case Notice::BackupDone:
        message = StrId::STR_STATS_BACKUP_DONE;
        break;
      case Notice::RestoreDone:
        message = StrId::STR_STATS_RESTORE_DONE;
        break;
      case Notice::NoBackup:
        message = StrId::STR_STATS_NO_BACKUP;
        break;
      case Notice::RestoreFailed:
        message = StrId::STR_STATS_RESTORE_FAILED;
        break;
      case Notice::BackupFailed:
      case Notice::None:
        break;
    }
    GUI.drawPopup(renderer, I18N.get(message));
    return;
  }
  renderer.displayBuffer();
}
