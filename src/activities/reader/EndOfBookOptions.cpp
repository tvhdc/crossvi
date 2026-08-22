#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <utility>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace {
// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string metricValue(const bool available, const uint32_t value) {
  return available ? std::to_string(value) : "--";
}

void drawMetric(const GfxRenderer& renderer, const Rect& rect, const std::string& value, const char* label) {
  const int valueHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int top = rect.y + std::max(2, (rect.height - valueHeight - labelHeight - 2) / 2);
  const std::string displayedValue = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), rect.width - 8,
                                                            EpdFontFamily::BOLD);
  const std::string displayedLabel = renderer.truncatedText(SMALL_FONT_ID, label, rect.width - 8);
  UITheme::drawCenteredText(renderer, rect, UI_10_FONT_ID, top, displayedValue.c_str(), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, rect, SMALL_FONT_ID, top + valueHeight + 2, displayedLabel.c_str());
}

void drawSummaryMetrics(const GfxRenderer& renderer, const Rect& rect, const EndOfBookSummary& summary,
                        const int columns) {
  char duration[28] = "--";
  if (summary.statsTrusted && !summary.stats.readingTimeUnavailable) {
    BookReadingStats::formatDuration(summary.stats.totalReadingSeconds, duration, sizeof(duration));
  }

  std::string completionDays = "--";
  if (summary.statsTrusted && summary.stats.isCompleted && summary.stats.startDate.isValid() &&
      summary.stats.finishedDate.isValid() &&
      compareReadingStatsDate(summary.stats.finishedDate, summary.stats.startDate) >= 0) {
    completionDays = std::to_string(readingSpanDaysInclusive(summary.stats.startDate, summary.stats.finishedDate));
  }

  const std::array<std::pair<std::string, const char*>, 4> metrics = {{
      {duration, tr(STR_STATS_READING_TIME)},
      {metricValue(summary.statsTrusted && !summary.stats.sessionsUnavailable, summary.stats.sessionCount),
       tr(STR_STATS_SESSIONS)},
      {metricValue(summary.statsTrusted && !summary.stats.pageTurnsUnavailable, summary.stats.totalPagesTurned),
       tr(STR_STATS_PAGES_TURNED)},
      {completionDays, tr(STR_STATS_DAYS_TO_FINISH)},
  }};

  const int rows = static_cast<int>((metrics.size() + columns - 1) / columns);
  const int rowHeight = rect.height / rows;
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 5, true);
  for (size_t index = 0; index < metrics.size(); ++index) {
    const int row = static_cast<int>(index) / columns;
    const int column = static_cast<int>(index) % columns;
    const int cellWidth = rect.width / columns;
    const Rect cell{rect.x + column * cellWidth, rect.y + row * rowHeight,
                    column == columns - 1 ? rect.width - column * cellWidth : cellWidth,
                    row == rows - 1 ? rect.height - row * rowHeight : rowHeight};
    if (column > 0) renderer.drawLine(cell.x, cell.y, cell.x, cell.y + cell.height - 1);
    if (row > 0 && column == 0) {
      renderer.drawLine(rect.x, cell.y, rect.x + rect.width - 1, cell.y);
    }
    drawMetric(renderer, cell, metrics[index].first, metrics[index].second);
  }
}
}  // namespace

bool EndOfBookOptions::start(const std::string& currentBookPath) {
  if (isStarted.load(std::memory_order_acquire)) return false;
  folder = FsHelpers::extractFolderPath(currentBookPath);
  selector.store(0, std::memory_order_relaxed);
  suggestionsReady.store(!suggestionScan.begin(currentBookPath, MAX_SUGGESTIONS), std::memory_order_relaxed);
  isStarted.store(true, std::memory_order_release);
  return true;
}

bool EndOfBookOptions::stepSuggestions(const size_t maxEntries) {
  if (!isStarted.load(std::memory_order_acquire) || suggestionsReady.load(std::memory_order_acquire)) return false;
  const NextBookFinder::StepResult result = suggestionScan.step(maxEntries);
  if (result == NextBookFinder::StepResult::Pending) return false;
  suggestionsReady.store(true, std::memory_order_release);
  return true;
}

bool EndOfBookOptions::menuActive() const { return isStarted.load(std::memory_order_acquire); }

const std::vector<std::string>& EndOfBookOptions::names() const {
  static const std::vector<std::string> empty;
  return suggestionsReady.load(std::memory_order_acquire) ? suggestionScan.result() : empty;
}

std::string EndOfBookOptions::fullPath(const size_t index) const {
  const auto& suggestions = names();
  if (index >= suggestions.size()) {
    return {};
  }
  return folder == "/" ? "/" + suggestions[index] : folder + "/" + suggestions[index];
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  const auto& suggestions = names();
  const int selected = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selected == 0) return Action::ViewStats;
    if (selected == 1) return Action::GoHome;
    const size_t suggestionIndex = static_cast<size_t>(selected - 2);
    if (suggestionIndex < suggestions.size()) {
      if (openPath) {
        *openPath = fullPath(suggestionIndex);
      }
      return Action::OpenBook;
    }
    return Action::None;
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) &&
      input.getHeldTime(MappedInputManager::Button::Back) < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = static_cast<int>(suggestions.size()) + 2;  // statistics, Home, then suggestions
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector.store(ButtonNavigator::previousIndex(selected, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector.store(ButtonNavigator::nextIndex(selected, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input,
                              const EndOfBookSummary& summary) const {
  const auto& suggestions = names();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Summary, actions and optional next-book suggestions. The hints are drawn at
  // the physical front buttons, which is a logical side/top edge in the rotated
  // orientations — lay out inside the safe area so nothing hides behind them. Vertical
  // positions derive from the safe-area height and font line heights so other panel
  // resolutions scale (review request on #2532).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + metrics.verticalSpacing;
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, titleY, tr(STR_STATS_FINISHED), true,
                            EpdFontFamily::BOLD);

  const int bookTitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const std::string bookTitle = renderer.truncatedText(UI_12_FONT_ID,
                                                       summary.title.empty() ? tr(STR_END_OF_BOOK) : summary.title.c_str(),
                                                       safe.width - 16, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, bookTitleY, bookTitle.c_str(), true,
                            EpdFontFamily::BOLD);

  int summaryTop = bookTitleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  if (!summary.author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, summary.author.c_str(), safe.width - 16);
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, summaryTop, author.c_str());
    summaryTop += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  }

  const bool landscape = safe.width > safe.height;
  const int summaryHeight = landscape ? 54 : 96;
  drawSummaryMetrics(renderer, Rect{safe.x + 4, summaryTop, safe.width - 8, summaryHeight}, summary,
                     landscape ? 4 : 2);
  const int listTop = summaryTop + summaryHeight + metrics.verticalSpacing * 2;

  const int listHeight = safe.y + safe.height - listTop - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{safe.x, listTop, safe.width, listHeight}, static_cast<int>(suggestions.size()) + 2,
               selector.load(std::memory_order_relaxed), [&suggestions](const int index) {
                 if (index == 0) return std::string(tr(STR_READING_STATS));
                 if (index == 1) return std::string(tr(STR_EOB_HOME));
                 return displayName(suggestions[static_cast<size_t>(index - 2)]);
               });

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
