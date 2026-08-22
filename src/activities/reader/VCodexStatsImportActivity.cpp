#include "VCodexStatsImportActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "CrossPointSettings.h"
#include "HalDisplay.h"
#include "ReadingAchievements.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const char* summaryValue(const bool available, const uint32_t value, char* buffer, const size_t size) {
  if (!available) return tr(STR_VCODEX_IMPORT_NO_DATA);
  snprintf(buffer, size, "%lu", static_cast<unsigned long>(value));
  return buffer;
}

void formatDuration(const VCodexStatsImportSummary& summary, char* buffer, const size_t size) {
  if (!summary.hasReadingTime) {
    snprintf(buffer, size, "%s", tr(STR_VCODEX_IMPORT_NO_DATA));
    return;
  }
  const uint32_t hours = summary.totalReadingSeconds / 3600u;
  const uint32_t minutes = summary.totalReadingSeconds % 3600u / 60u;
  snprintf(buffer, size, "%luh %02lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
}
}  // namespace

VCodexStatsImportActivity::VCodexStatsImportActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const VCodexStatsImporter::ProbeResult probeResult,
                                                     const VCodexStatsImportSummary& summary,
                                                     std::unique_ptr<Activity> nextActivity)
    : Activity("VCodexStatsImport", renderer, mappedInput),
      summary_(summary),
      nextActivity_(std::move(nextActivity)),
      probeResult_(probeResult) {}

std::unique_ptr<VCodexStatsImportActivity> VCodexStatsImportActivity::forManualImport(GfxRenderer& renderer,
                                                                                      MappedInputManager& mappedInput) {
  VCodexStatsImportSummary summary;
  const VCodexStatsImporter::ProbeResult result = VCodexStatsImporter::probeManual(summary);
  return std::make_unique<VCodexStatsImportActivity>(renderer, mappedInput, result, summary);
}

void VCodexStatsImportActivity::onEnter() {
  Activity::onEnter();
  initialInputReleased_ = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                          !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                          !mappedInput.isPressed(MappedInputManager::Button::Left) &&
                          !mappedInput.isPressed(MappedInputManager::Button::Right);

  if (probeResult_ == VCodexStatsImporter::ProbeResult::PendingRecovery) {
    showBlockingFeedback(StrId::STR_VCODEX_IMPORTING);
    const int16_t offsetMinutes =
        static_cast<int16_t>((static_cast<int>(std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104)) - 48) * 15);
    setImportResult(VCodexStatsImporter::recoverPending(offsetMinutes));
  } else if (probeResult_ == VCodexStatsImporter::ProbeResult::CrossViNotEmpty) {
    state_ = State::NotEmpty;
  } else if (probeResult_ != VCodexStatsImporter::ProbeResult::Offer) {
    state_ = State::NotAvailable;
  }
  requestUpdate();
}

void VCodexStatsImportActivity::setImportResult(const VCodexStatsImporter::ImportResult result) {
  state_ = result == VCodexStatsImporter::ImportResult::Imported   ? State::Success
           : result == VCodexStatsImporter::ImportResult::NotEmpty ? State::NotEmpty
                                                                   : State::Failed;
  if (result == VCodexStatsImporter::ImportResult::Imported) {
    ReadingAchievementNotification notification;
    if (ReadingAchievements::peekPendingNotification(notification)) unlockedAchievements_ = notification.count;
  }
}

void VCodexStatsImportActivity::continueAfterResult() {
  if (unlockedAchievements_ != 0) ReadingAchievements::ackPendingNotification();
  if (nextActivity_) {
    activityManager.replaceActivity(std::move(nextActivity_));
  } else {
    finish();
  }
}

void VCodexStatsImportActivity::loop() {
  if (!initialInputReleased_) {
    initialInputReleased_ = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                            !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                            !mappedInput.isPressed(MappedInputManager::Button::Left) &&
                            !mappedInput.isPressed(MappedInputManager::Button::Right);
    return;
  }

  if (state_ != State::Prompt) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      continueAfterResult();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    showBlockingFeedback(StrId::STR_PROCESSING);
    const VCodexStatsImporter::ImportResult result = VCodexStatsImporter::decline();
    if (result == VCodexStatsImporter::ImportResult::Declined) {
      continueAfterResult();
    } else {
      setImportResult(result);
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showBlockingFeedback(StrId::STR_VCODEX_IMPORTING);
    const int16_t offsetMinutes =
        static_cast<int16_t>((static_cast<int>(std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104)) - 48) * 15);
    setImportResult(VCodexStatsImporter::import(offsetMinutes));
    requestUpdate();
  }
}

void VCodexStatsImportActivity::render(RenderLock&&) {
  if (renderBlockingFeedbackOverlay()) return;
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int contentX = metrics.contentSidePadding;
  const int contentWidth = width - 2 * contentX;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VCODEX_IMPORT_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const char* message = nullptr;
  switch (state_) {
    case State::Success:
      message = tr(STR_VCODEX_IMPORT_SUCCESS);
      break;
    case State::Failed:
      message = tr(STR_VCODEX_IMPORT_FAILED);
      break;
    case State::NotAvailable:
      message = tr(STR_VCODEX_IMPORT_NOT_AVAILABLE);
      break;
    case State::NotEmpty:
      message = tr(STR_VCODEX_IMPORT_NOT_EMPTY);
      break;
    case State::Prompt:
      message = tr(STR_VCODEX_IMPORT_PROMPT);
      break;
  }
  for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, message, contentWidth, 4)) {
    renderer.drawText(UI_10_FONT_ID, contentX, y, line.c_str(), true,
                      state_ == State::Prompt ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  if (state_ == State::Success && unlockedAchievements_ != 0) {
    y += metrics.verticalSpacing * 2;
    char unlocked[96];
    snprintf(unlocked, sizeof(unlocked), tr(STR_ACHIEVEMENTS_HISTORY_UNLOCKED_FORMAT),
             static_cast<unsigned>(unlockedAchievements_));
    renderer.drawText(UI_10_FONT_ID, contentX, y, unlocked, true, EpdFontFamily::BOLD);
  }

  if (state_ == State::Prompt) {
    y += metrics.verticalSpacing * 2;
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
    const int boxHeight = rowHeight * 5 + metrics.verticalSpacing;
    renderer.drawRect(contentX, y, contentWidth, boxHeight, true);
    int rowY = y + metrics.verticalSpacing;
    char duration[32];
    char value[32];
    formatDuration(summary_, duration, sizeof(duration));
    auto drawRow = [&](const char* label, const char* displayed) {
      renderer.drawText(UI_10_FONT_ID, contentX + 10, rowY, label);
      const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, displayed);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 10 - valueWidth, rowY, displayed, true,
                        EpdFontFamily::BOLD);
      rowY += rowHeight;
    };
    drawRow(tr(STR_STATS_READING_TIME), duration);
    drawRow(tr(STR_STATS_SESSIONS), summaryValue(summary_.hasSessions, summary_.totalSessions, value, sizeof(value)));
    drawRow(tr(STR_STATS_COMPLETED_BOOKS),
            summaryValue(summary_.hasCompletedBooks, summary_.completedBooks, value, sizeof(value)));
    drawRow(tr(STR_VCODEX_IMPORT_CALENDAR_DAYS),
            summaryValue(summary_.hasCalendarDays, summary_.calendarDays, value, sizeof(value)));
    drawRow(tr(STR_VCODEX_IMPORT_BOOKS),
            summaryValue(summary_.hasResolvableBooks, summary_.resolvableBooks, value, sizeof(value)));
  }

  if (state_ == State::Prompt) {
    const auto labels = mappedInput.mapLabels("", "", tr(STR_NO), tr(STR_YES));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONTINUE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
