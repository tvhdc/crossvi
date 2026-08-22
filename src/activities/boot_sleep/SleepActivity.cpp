#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/SourceIdentityStore.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DitherUtils.h"
#include "Memory.h"
#include "RecentBooksStore.h"
#include "SleepImagePlacement.h"
#include "SleepFrameStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsLoader.h"
#include "activities/reader/DailyReadingHistory.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressFileCodec.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/ReadingCalendarLayout.h"
#include "activities/reader/ReadingCalendarModel.h"
#include "activities/reader/ReadingCalendarRenderer.h"
#include "activities/reader/ReadingStatsPresentation.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/DefaultSleepScreens.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {
// A sleep render is the last panel operation before the MCU enters deep sleep.
// Power the panel down as part of that refresh so teardown work cannot leave it
// electrically driven and darken the image after it has settled.
constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
// Grayscale sleep covers still need one clean base conditioning pass before
// their gray planes are written. Plain B/W sleep frames finish with the OEM
// full waveform itself, followed immediately by panel power-off.
constexpr uint8_t X3_SLEEP_CONDITION_PASSES = 1;

void prepareStrongSleepRefresh() { display.requestResync(X3_SLEEP_CONDITION_PASSES); }

void runFastSleepCleanupPass() {
  if (!display.supportsX3GhostCleanup()) {
    // X4 and X3 controllers without the validated cleanup LUT keep the stock
    // black-flash waveform as their safe equivalent.
    LOG_INF("SLW", "sleep ghost fast-clean using half fallback");
    display.displayBuffer(HalDisplay::HALF_REFRESH, false);
    return;
  }

  display.displayBuffer(HalDisplay::FAST_REFRESH, false);
  const bool cleanupStarted = display.cleanX3GhostingNow();
  LOG_INF("SLW", "sleep ghost fast-clean x3 started=%u", static_cast<unsigned>(cleanupStarted));
  if (!cleanupStarted) {
    // A pending initial resync can promote FAST to FULL, which deliberately
    // makes the X3 cleanup LUT ineligible. Fall back instead of silently
    // weakening the selected sleep treatment.
    display.displayBuffer(HalDisplay::HALF_REFRESH, false);
  }
}

void applySleepGhostingTreatment() {
  const uint8_t treatment = SETTINGS.sleepGhostingTreatment < CrossPointSettings::SLEEP_GHOSTING_TREATMENT_COUNT
                                ? SETTINGS.sleepGhostingTreatment
                                : CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL;
  LOG_INF("SLW", "sleep ghost treatment=%u pre-refresh begin", static_cast<unsigned>(treatment));

  switch (treatment) {
    case CrossPointSettings::SLEEP_GHOST_FAST_FULL:
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_TWICE_FULL:
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL:
      runFastSleepCleanupPass();
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_TWICE_FULL:
      runFastSleepCleanupPass();
      runFastSleepCleanupPass();
      break;
    case CrossPointSettings::SLEEP_GHOST_HALF_FULL:
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_HALF_TWICE_FULL:
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_TWICE:
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_THREE_TIMES:
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_ONLY:
    default:
      break;
  }
}

void displayStrongSleepFrame() {
  const unsigned long startedAt = millis();
  applySleepGhostingTreatment();
  LOG_INF("SLW", "sleep-frame terminal full refresh begin turn_off=1");
  // FULL_REFRESH remains mandatory. On X3 the OEM full waveform is the final
  // physical panel update; the driver powers the panel off as soon as it ends.
  display.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  LOG_INF("SLW", "sleep-frame full refresh complete elapsed_ms=%lu", millis() - startedAt);
}

void drawCenteredInRect(const GfxRenderer& renderer, const int fontId, const Rect& rect, const int y, const char* text,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rect.x + std::max(0, (rect.width - width) / 2), y, text, true, style);
}

void drawCalendarMetric(const GfxRenderer& renderer, const Rect& rect, const std::string& value, const StrId label,
                        const int valueFontId = UI_10_FONT_ID) {
  const int valueHeight = renderer.getLineHeight(valueFontId);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int top = rect.y + std::max(0, (rect.height - valueHeight - labelHeight - 4) / 2);
  const std::string shownValue =
      renderer.truncatedText(valueFontId, value.c_str(), rect.width - 8, EpdFontFamily::BOLD);
  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, I18N.get(label), rect.width - 8);
  drawCenteredInRect(renderer, valueFontId, rect, top, shownValue.c_str(), EpdFontFamily::BOLD);
  drawCenteredInRect(renderer, SMALL_FONT_ID, rect, top + valueHeight + 4, shownLabel.c_str());
}

void drawCalendarWeekdays(const GfxRenderer& renderer, const ReadingCalendarGridLayout& layout) {
  constexpr std::array<StrId, ReadingCalendarGridLayout::COLUMNS> labels = {
      StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
      StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};
  for (size_t index = 0; index < labels.size(); ++index) {
    const Rect column{layout.grid.x + static_cast<int>(index) * (layout.cellSize + ReadingCalendarGridLayout::CELL_GAP),
                      layout.weekdays.y, layout.cellSize, layout.weekdays.height};
    drawCenteredInRect(renderer, SMALL_FONT_ID, column, column.y, I18N.get(labels[index]), EpdFontFamily::BOLD);
  }
}

struct SleepBookSummary {
  std::string title;
  std::string author;
  std::string chapter;
  std::string readingTime;
  std::string sessions;
  std::string pagesTurned;
  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  bool progressBelowOnePercent = false;
  bool available = false;
};

std::string bookTitleFromPath(const std::string& path) {
  const size_t separator = path.find_last_of('/');
  std::string title = separator == std::string::npos ? path : path.substr(separator + 1);
  const size_t extension = title.find_last_of('.');
  if (extension != std::string::npos && extension > 0) title.resize(extension);
  return title;
}

std::string formatSleepDuration(const uint32_t seconds, const bool estimated) {
  char value[28];
  const char* prefix = estimated ? "~" : "";
  if (seconds == 0) {
    snprintf(value, sizeof(value), "%s0m", prefix);
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "%s<1m", prefix);
  } else {
    const uint32_t minutes = seconds / 60;
    if (minutes < 60) {
      snprintf(value, sizeof(value), "%s%lum", prefix, static_cast<unsigned long>(minutes));
    } else {
      const uint32_t hours = minutes / 60;
      const uint32_t remainder = minutes % 60;
      if (hours < 1000 && remainder > 0) {
        snprintf(value, sizeof(value), "%s%luh %lum", prefix, static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(remainder));
      } else {
        snprintf(value, sizeof(value), "%s%luh", prefix, static_cast<unsigned long>(hours));
      }
    }
  }
  return value;
}

std::string formatSleepMetric(const ReadingStatsMetric& metric, const bool duration) {
  switch (metric.state) {
    case ReadingStatsMetricState::NotApplicable:
      return tr(STR_STATS_NOT_APPLICABLE);
    case ReadingStatsMetricState::NoData:
      return tr(STR_STATS_NO_DATA);
    case ReadingStatsMetricState::Unavailable:
      return tr(STR_STATS_UNAVAILABLE);
    case ReadingStatsMetricState::Known:
    case ReadingStatsMetricState::Estimated:
      if (duration) return formatSleepDuration(metric.value, metric.state == ReadingStatsMetricState::Estimated);
      return std::string(metric.state == ReadingStatsMetricState::Estimated ? "~" : "") + std::to_string(metric.value);
  }
  return tr(STR_STATS_UNAVAILABLE);
}

std::string formatSleepProgress(const SleepBookSummary& summary) {
  if (summary.progress.state != ReadingStatsMetricState::Known &&
      summary.progress.state != ReadingStatsMetricState::Estimated) {
    return formatSleepMetric(summary.progress, false);
  }
  const char* prefix = summary.progress.state == ReadingStatsMetricState::Estimated ? "~" : "";
  if (summary.progressBelowOnePercent) return std::string(prefix) + "<1%";
  return std::string(prefix) + std::to_string(std::min<uint32_t>(summary.progress.value, 100)) + "%";
}

void setSleepProgress(SleepBookSummary& summary, const uint8_t percent, const bool estimated = false,
                      const bool belowOnePercent = false) {
  summary.progress = estimated ? ReadingStatsMetric::estimated(percent) : ReadingStatsMetric::known(percent);
  summary.progressBelowOnePercent = belowOnePercent;
}

bool hasTrustedSourceIdentity(const std::string& cachePath, const ZipFile::SourceIdentity& current) {
  ZipFile::SourceIdentity stored;
  const SourceIdentityStore::LoadStatus status = SourceIdentityStore::load(cachePath, stored);
  return (status == SourceIdentityStore::LoadStatus::Primary || status == SourceIdentityStore::LoadStatus::Backup ||
          status == SourceIdentityStore::LoadStatus::Temp) &&
         stored == current;
}

void loadEpubSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Epub book(recent.path, "/.crosspoint");
  if (book.inspectCache() != BookMetadataCache::LoadStatus::Loaded) return;

  std::array<uint8_t, ProgressFile::EPUB_CONTENT_ANCHORED_PROGRESS_SIZE> bytes{};
  const ProgressFile::EpubBounds bounds{static_cast<uint32_t>(book.getSpineItemsCount())};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateEpubBounds, &bounds};
  const ProgressFile::LoadResult loaded =
      ProgressFile::loadEpub(book.getCachePath(), bytes.data(), bytes.size(), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  const uint16_t spineIndex = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
  const int tocIndex = book.getTocIndexForSpineIndex(spineIndex);
  if (tocIndex >= 0) summary.chapter = book.getTocItem(tocIndex).title;

  DashboardProgress::Position position;
  if (!DashboardProgress::decode(bytes.data(), loaded.size, position)) return;
  const float chapterProgress = static_cast<float>(position.pageNumber + 1U) / position.pageCount;
  float bookProgress = 0.0F;
  uint8_t percent = 0;
  if (book.calculateProgressChecked(position.spineIndex, chapterProgress, bookProgress) &&
      DashboardProgress::toPercent(bookProgress, percent)) {
    setSleepProgress(summary, percent, false, bookProgress > 0.0F && percent == 0);
  }
}

void loadXtcSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Xtc book(recent.path, "/.crosspoint");
  ZipFile::SourceIdentity identity;
  if (!book.load() || !book.getSourceIdentity(identity) || !hasTrustedSourceIdentity(book.getCachePath(), identity)) {
    return;
  }

  uint8_t bytes[4]{};
  const ProgressFile::PageBounds bounds{book.getPageCount()};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
  const ProgressFile::LoadResult loaded = ProgressFile::loadPage(book.getCachePath(), bytes, sizeof(bytes), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  const uint32_t page = ProgressFileCodec::decodeU32(bytes);
  setSleepProgress(summary, book.calculateProgress(page));
  if (!book.hasChapters()) return;
  const auto& chapters = book.getChapters();
  const auto chapter = std::find_if(chapters.begin(), chapters.end(), [page](const xtc::ChapterInfo& entry) {
    return page >= entry.startPage && page <= entry.endPage;
  });
  if (chapter != chapters.end()) summary.chapter = chapter->name;
}

void loadTxtSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Txt book(recent.path, "/.crosspoint");
  ZipFile::SourceIdentity identity;
  if (!book.load() || !book.getSourceIdentity(identity) || !hasTrustedSourceIdentity(book.getCachePath(), identity) ||
      book.getFileSize() == 0 || book.getFileSize() > UINT32_MAX) {
    return;
  }

  uint8_t bytes[ProgressFileCodec::TXT_V2_SIZE]{};
  const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(book.getFileSize()), 0};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  const ProgressFile::LoadResult loaded = ProgressFile::loadTxt(book.getCachePath(), bytes, sizeof(bytes), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  uint32_t byteOffset = 0;
  if (ProgressFileCodec::decodeTxt(bytes, loaded.size, byteOffset) != ProgressFileCodec::TxtDecodeStatus::Ok) return;
  const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(byteOffset) * 100U / book.getFileSize());
  setSleepProgress(summary, static_cast<uint8_t>(std::min<uint32_t>(percent, 100)), true,
                   byteOffset > 0 && percent == 0);
}

void loadSleepBookPosition(const RecentBook& recent, SleepBookSummary& summary) {
  if (FsHelpers::hasEpubExtension(recent.path)) {
    loadEpubSleepPosition(recent, summary);
  } else if (FsHelpers::hasXtcExtension(recent.path)) {
    loadXtcSleepPosition(recent, summary);
  } else if (FsHelpers::hasTxtExtension(recent.path) || FsHelpers::hasMarkdownExtension(recent.path)) {
    loadTxtSleepPosition(recent, summary);
  }
}

SleepBookSummary loadSleepBookSummary() {
  SleepBookSummary summary;
  const std::string& path = APP_STATE.openEpubPath;
  if (path.empty() || !Storage.exists(path.c_str())) return summary;

  RecentBook recent{path, bookTitleFromPath(path), "", ""};
  const auto& books = RECENT_BOOKS.getBooks();
  const auto found =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& book) { return book.path == path; });
  if (found != books.end()) recent = *found;
  if (recent.title.empty()) recent.title = bookTitleFromPath(path);

  summary.title = recent.title;
  summary.author = recent.author.empty() ? tr(STR_STATS_NO_DATA) : recent.author;
  summary.available = !summary.title.empty();
  if (!summary.available) return summary;

  loadSleepBookPosition(recent, summary);

  BookReadingStats stats;
  if (!loadTrustedBookReadingStats(recent, stats)) {
    summary.readingTime = tr(STR_STATS_UNAVAILABLE);
    summary.sessions = tr(STR_STATS_UNAVAILABLE);
    summary.pagesTurned = tr(STR_STATS_UNAVAILABLE);
    return summary;
  }
  if (stats.isCompleted && !stats.completionUnavailable) setSleepProgress(summary, 100);
  summary.readingTime =
      formatSleepMetric(stats.readingTimeUnavailable ? ReadingStatsMetric::noData()
                                                     : ReadingStatsMetric::known(stats.totalReadingSeconds),
                        true);
  summary.sessions = formatSleepMetric(
      stats.sessionsUnavailable ? ReadingStatsMetric::noData() : ReadingStatsMetric::known(stats.sessionCount), false);
  summary.pagesTurned = formatSleepMetric(
      stats.pageTurnsUnavailable ? ReadingStatsMetric::noData() : ReadingStatsMetric::known(stats.totalPagesTurned),
      false);
  return summary;
}

Rect drawSleepBookStatsOverlay(const GfxRenderer& renderer, const SleepBookSummary& summary) {
  if (!summary.available) return Rect{};

  constexpr int cardMargin = 16;
  constexpr int topPadding = 16;
  constexpr int leftPadding = 18;
  constexpr int rightPadding = 14;
  constexpr int titleAuthorGap = 8;
  constexpr int authorChapterGap = 8;
  constexpr int detailsGap = 10;
  constexpr int progressBarHeight = 10;
  constexpr int progressStatsGap = 12;
  constexpr int statsHeight = 76;
  constexpr int bottomPadding = 11;
  const int cardWidth = renderer.getScreenWidth() - cardMargin * 2;
  const int contentWidth = cardWidth - leftPadding - rightPadding;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int detailsLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, summary.title.c_str(), contentWidth, 2, EpdFontFamily::BOLD);
  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int authorYInCard = topPadding + titleBlockHeight + titleAuthorGap;
  const int chapterYInCard = authorYInCard + authorLineHeight + authorChapterGap;
  const int progressYInCard = summary.chapter.empty() ? authorYInCard + authorLineHeight + detailsGap
                                                      : chapterYInCard + detailsLineHeight + detailsGap;
  const int progressBarYInCard = progressYInCard + detailsLineHeight + 4;
  const int statsTopInCard = progressBarYInCard + progressBarHeight + progressStatsGap;
  const int cardHeight = statsTopInCard + 1 + statsHeight + bottomPadding;
  const Rect card{cardMargin, renderer.getScreenHeight() - cardMargin - cardHeight, cardWidth, cardHeight};

  renderer.fillRect(card.x, card.y, card.width, card.height, false);
  renderer.drawRect(card.x, card.y, card.width, card.height, 2, true);
  renderer.fillRect(card.x + 2, card.y + 2, 5, card.height - 4, true);

  const int contentX = card.x + leftPadding;
  int y = card.y + topPadding;

  for (const std::string& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, contentX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }
  y = card.y + authorYInCard;
  const std::string author = renderer.truncatedText(UI_10_FONT_ID, summary.author.c_str(), contentWidth);
  renderer.drawText(UI_10_FONT_ID, contentX, y, author.c_str());

  if (!summary.chapter.empty()) {
    const std::string chapterLabel = std::string(tr(STR_CHAPTER_PREFIX)) + summary.chapter;
    const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, chapterLabel.c_str(), contentWidth);
    renderer.drawText(UI_10_FONT_ID, contentX, card.y + chapterYInCard, chapter.c_str());
  }

  const std::string progressValue = formatSleepProgress(summary);
  renderer.drawText(UI_10_FONT_ID, contentX, card.y + progressYInCard, tr(STR_STATS_PROGRESS), true,
                    EpdFontFamily::BOLD);
  const int progressValueWidth = renderer.getTextWidth(UI_10_FONT_ID, progressValue.c_str());
  renderer.drawText(UI_10_FONT_ID, contentX + std::max(0, contentWidth - progressValueWidth), card.y + progressYInCard,
                    progressValue.c_str());
  const int progressBarY = card.y + progressBarYInCard;
  renderer.drawRoundedRect(contentX, progressBarY, contentWidth, progressBarHeight, 1, 3, true);
  if (summary.progress.state == ReadingStatsMetricState::Known ||
      summary.progress.state == ReadingStatsMetricState::Estimated) {
    const int fillWidth = DashboardProgress::fillWidth(contentWidth, static_cast<uint8_t>(summary.progress.value));
    if (fillWidth > 0) {
      renderer.fillRoundedRect(contentX + 2, progressBarY + 2, fillWidth, progressBarHeight - 4, 1, Color::Black);
    }
  }

  const int statsTop = card.y + statsTopInCard;
  renderer.drawLine(contentX, statsTop, contentX + contentWidth - 1, statsTop);
  const Rect stats{contentX, statsTop + 1, contentWidth, statsHeight};
  const int firstWidth = stats.width * 40 / 100;
  const int secondWidth = stats.width * 25 / 100;
  renderer.drawLine(stats.x + firstWidth, stats.y + 8, stats.x + firstWidth, stats.y + stats.height - 8);
  renderer.drawLine(stats.x + firstWidth + secondWidth, stats.y + 8, stats.x + firstWidth + secondWidth,
                    stats.y + stats.height - 8);
  drawCalendarMetric(renderer, Rect{stats.x, stats.y, firstWidth, stats.height}, summary.readingTime,
                     StrId::STR_STATS_READING_TIME, UI_12_FONT_ID);
  drawCalendarMetric(renderer, Rect{stats.x + firstWidth, stats.y, secondWidth, stats.height}, summary.sessions,
                     StrId::STR_STATS_SESSIONS);
  drawCalendarMetric(
      renderer, Rect{stats.x + firstWidth + secondWidth, stats.y, stats.width - firstWidth - secondWidth, stats.height},
      summary.pagesTurned, StrId::STR_STATS_PAGES_TURNED);
  return card;
}

Rect drawSleepBookStatsOverlay(const GfxRenderer& renderer) {
  return drawSleepBookStatsOverlay(renderer, loadSleepBookSummary());
}

struct PopupSnapshot {
  Rect rect{};
  std::unique_ptr<uint8_t[]> bytes;
  size_t size = 0;
};

Rect enteringSleepPopupOuterRect(const GfxRenderer& renderer, const char* message) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const EpdFontFamily::Style style = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, style);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + metrics.popupMarginX * 2;
  const int h = textHeight + metrics.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  return Rect{x - metrics.popupFrameThickness, y - metrics.popupFrameThickness, w + metrics.popupFrameThickness * 2,
              h + metrics.popupFrameThickness * 2};
}

PopupSnapshot capturePopupSnapshot(const GfxRenderer& renderer, const char* message) {
  PopupSnapshot snapshot;
  snapshot.rect = enteringSleepPopupOuterRect(renderer, message);
  snapshot.size =
      renderer.getRegionByteSize(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height);
  if (snapshot.size == 0) return snapshot;
  snapshot.bytes = makeUniqueNoThrow<uint8_t[]>(snapshot.size);
  if (!snapshot.bytes) {
    snapshot.size = 0;
    return snapshot;
  }
  if (!renderer.copyRegionToBuffer(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height,
                                   snapshot.bytes.get(), snapshot.size)) {
    snapshot.bytes.reset();
    snapshot.size = 0;
  }
  return snapshot;
}

void restorePopupSnapshot(const GfxRenderer& renderer, const PopupSnapshot& snapshot) {
  if (!snapshot.bytes || snapshot.size == 0) return;
  renderer.copyBufferToRegion(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height,
                              snapshot.bytes.get(), snapshot.size);
}

void showEnteringSleepPopup(const GfxRenderer& renderer, const char* message, const bool transparent) {
  if (!transparent) {
    GUI.drawPopup(renderer, message);
    return;
  }
  const PopupSnapshot snapshot = capturePopupSnapshot(renderer, message);
  if (!snapshot.bytes || snapshot.size == 0) {
    LOG_ERR("SLP", "Skipping transparent sleep popup: snapshot unavailable");
    return;
  }
  GUI.drawPopup(renderer, message);
  restorePopupSnapshot(renderer, snapshot);
}

void drawMoonOnCurrentFrame(const GfxRenderer& renderer) {
  const int pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
}

SleepImagePlacement placeSleepImage(const GfxRenderer& renderer, const int sourceWidth, const int sourceHeight) {
  return calculateSleepImagePlacement(renderer.getScreenWidth(), renderer.getScreenHeight(), sourceWidth, sourceHeight,
                                      SETTINGS.sleepScreenImageZoom, SETTINGS.sleepScreenImageOffsetX,
                                      SETTINGS.sleepScreenImageOffsetY);
}

bool alphaCoveragePasses(const uint8_t alpha, const int x, const int y) {
  if (alpha == 0) return false;
  if (alpha == 255) return true;
  return alpha > static_cast<uint8_t>(bayer4x4[y & 3][x & 3] * 16U);
}

bool readLE16(HalFile& file, uint16_t& value) {
  uint8_t raw[2];
  if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  value = static_cast<uint16_t>(raw[0]) | static_cast<uint16_t>(raw[1]) << 8U;
  return true;
}

bool readLE32(HalFile& file, uint32_t& value) {
  uint8_t raw[4];
  if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  value = static_cast<uint32_t>(raw[0]) | static_cast<uint32_t>(raw[1]) << 8U | static_cast<uint32_t>(raw[2]) << 16U |
          static_cast<uint32_t>(raw[3]) << 24U;
  return true;
}

struct Bmp32OverlayHeader {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint64_t pixelOffset = 0;
  uint32_t rowBytes = 0;
};

enum class Bmp32HeaderStatus : uint8_t { Invalid, Not32Bit, Valid };

Bmp32HeaderStatus readBmp32OverlayHeader(HalFile& file, Bmp32OverlayHeader& out) {
  if (!file.seek(0)) return Bmp32HeaderStatus::Invalid;
  const uint64_t fileSize = file.fileSize64();
  uint16_t bfType = 0;
  uint32_t declaredFileSize = 0;
  uint32_t pixelOffset = 0;
  if (!readLE16(file, bfType) || !readLE32(file, declaredFileSize) || !file.seekCur(4) ||
      !readLE32(file, pixelOffset)) {
    return Bmp32HeaderStatus::Invalid;
  }
  if (bfType != 0x4D42) return Bmp32HeaderStatus::Invalid;

  uint32_t dibSize = 0;
  uint32_t rawWidth = 0;
  uint32_t rawHeightBits = 0;
  uint16_t planes = 0;
  uint16_t bpp = 0;
  uint32_t compression = 0;
  if (!readLE32(file, dibSize) || dibSize < 40 || !readLE32(file, rawWidth) || !readLE32(file, rawHeightBits) ||
      !readLE16(file, planes) || !readLE16(file, bpp) || !readLE32(file, compression)) {
    return Bmp32HeaderStatus::Invalid;
  }
  if (bpp != 32) return Bmp32HeaderStatus::Not32Bit;

  const int32_t width = static_cast<int32_t>(rawWidth);
  const int32_t rawHeight = static_cast<int32_t>(rawHeightBits);
  if (width <= 0 || rawHeight == INT32_MIN || planes != 1) return Bmp32HeaderStatus::Invalid;
  const int32_t height = rawHeight < 0 ? -rawHeight : rawHeight;
  constexpr int MAX_OVERLAY_WIDTH = 2048;
  constexpr int MAX_OVERLAY_HEIGHT = 3072;
  if (height <= 0 || width > MAX_OVERLAY_WIDTH || height > MAX_OVERLAY_HEIGHT) return Bmp32HeaderStatus::Invalid;
  if (!(compression == 0 || compression == 3)) return Bmp32HeaderStatus::Invalid;

  if (compression == 3) {
    const uint64_t maskOffset = 14ULL + 40ULL;
    if (!file.seek64(maskOffset)) return Bmp32HeaderStatus::Invalid;
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;
    if (!readLE32(file, redMask) || !readLE32(file, greenMask) || !readLE32(file, blueMask) ||
        !readLE32(file, alphaMask)) {
      return Bmp32HeaderStatus::Invalid;
    }
    if (redMask != 0x00FF0000UL || greenMask != 0x0000FF00UL || blueMask != 0x000000FFUL || alphaMask != 0xFF000000UL) {
      return Bmp32HeaderStatus::Invalid;
    }
  }

  const uint64_t minimumPixelOffset = 14ULL + dibSize + (compression == 3 && dibSize == 40 ? 16ULL : 0ULL);
  if (pixelOffset < minimumPixelOffset) return Bmp32HeaderStatus::Invalid;
  const uint64_t rowBytes = static_cast<uint64_t>(width) * 4ULL;
  const uint64_t pixelBytes = rowBytes * static_cast<uint64_t>(height);
  if (rowBytes > std::numeric_limits<uint32_t>::max() || pixelOffset > fileSize ||
      pixelBytes > fileSize - pixelOffset) {
    return Bmp32HeaderStatus::Invalid;
  }
  if (declaredFileSize != 0 &&
      (declaredFileSize < pixelOffset || declaredFileSize > fileSize || pixelBytes > declaredFileSize - pixelOffset)) {
    return Bmp32HeaderStatus::Invalid;
  }

  out.width = width;
  out.height = height;
  out.topDown = rawHeight < 0;
  out.pixelOffset = pixelOffset;
  out.rowBytes = static_cast<uint32_t>(rowBytes);
  return Bmp32HeaderStatus::Valid;
}

bool renderBmp32Overlay(HalFile& file, const Bmp32OverlayHeader& header, GfxRenderer& renderer) {
  const SleepImagePlacement placement = placeSleepImage(renderer, header.width, header.height);
  if (placement.width <= 0 || placement.height <= 0) return false;
  auto row = makeUniqueNoThrow<uint8_t[]>(header.rowBytes);
  if (!row) return false;

  const int firstVisibleY = std::max(0, -placement.y);
  const int lastVisibleY = std::min(placement.height, renderer.getScreenHeight() - placement.y);
  const int firstVisibleX = std::max(0, -placement.x);
  const int lastVisibleX = std::min(placement.width, renderer.getScreenWidth() - placement.x);
  if (firstVisibleX >= lastVisibleX || firstVisibleY >= lastVisibleY) return true;

  for (int outY = firstVisibleY; outY < lastVisibleY; ++outY) {
    const int sourceY = std::min(header.height - 1, static_cast<int>(std::floor(outY / placement.scale)));
    const int fileRow = header.topDown ? sourceY : header.height - 1 - sourceY;
    const uint64_t rowOffset = header.pixelOffset + static_cast<uint64_t>(fileRow) * header.rowBytes;
    if (!file.seek64(rowOffset) || file.read(row.get(), header.rowBytes) != static_cast<int>(header.rowBytes)) {
      return false;
    }
    const int screenY = placement.y + outY;
    for (int outX = firstVisibleX; outX < lastVisibleX; ++outX) {
      const int sourceX = std::min(header.width - 1, static_cast<int>(std::floor(outX / placement.scale)));
      const uint8_t* pixel = row.get() + static_cast<size_t>(sourceX) * 4U;
      const uint8_t alpha = pixel[3];
      const int screenX = placement.x + outX;
      if (!alphaCoveragePasses(alpha, screenX, screenY)) continue;
      const uint8_t gray = static_cast<uint8_t>((pixel[2] * 77U + pixel[1] * 150U + pixel[0] * 29U) >> 8U);
      renderer.drawPixel(screenX, screenY, applyBayerDither1Bit(gray, screenX, screenY));
    }
  }
  return true;
}

bool validatePngOverlay(const std::string& path) {
  ImageDimensions dimensions{};
  return PngToFramebufferConverter::getDimensionsStatic(path, dimensions) && dimensions.width > 0 &&
         dimensions.height > 0;
}

bool validateBmpOverlay(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;
  Bmp32OverlayHeader alphaHeader;
  const Bmp32HeaderStatus alphaStatus = readBmp32OverlayHeader(file, alphaHeader);
  if (alphaStatus == Bmp32HeaderStatus::Valid) return file.close();
  if (alphaStatus == Bmp32HeaderStatus::Invalid) return file.close() && false;
  if (!file.seek(0)) return file.close() && false;
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
  return file.close() && valid;
}

bool isOverlayImageName(const std::string& filename) {
  return !filename.empty() && filename[0] != '.' && filename.size() <= 256 &&
         (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename));
}

bool validateOverlayImage(const std::string& path) {
  if (FsHelpers::hasPngExtension(path)) return validatePngOverlay(path);
  if (FsHelpers::hasBmpExtension(path)) return validateBmpOverlay(path);
  return false;
}

struct OverlayCandidate {
  std::string path;
  uint16_t index = 0;
  bool valid = false;
};

constexpr uint16_t MAX_SLEEP_DIRECTORY_ENTRIES = 4096;
constexpr uint8_t SLEEP_DIRECTORY_YIELD_INTERVAL = 16;

template <typename Validator>
OverlayCandidate directoryImageCandidate(const char* directoryPath, Validator&& validator) {
  HalFile dir = Storage.open(directoryPath);
  if (!dir || !dir.isDirectory()) return {};

  OverlayCandidate any;
  OverlayCandidate fresh;
  uint16_t entries = 0;
  uint16_t validCount = 0;
  uint16_t freshCount = 0;
  char name[257];
  for (HalFile file = dir.openNextFile(); file && entries < MAX_SLEEP_DIRECTORY_ENTRIES;
       file = dir.openNextFile()) {
    ++entries;
    if (entries % SLEEP_DIRECTORY_YIELD_INTERVAL == 0) yield();
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    const size_t length = file.getName(name, sizeof(name));
    file.close();
    if (length == 0 || length >= sizeof(name)) continue;
    const std::string filename(name);
    if (!isOverlayImageName(filename)) continue;
    const std::string path = std::string(directoryPath) + "/" + filename;
    if (!validator(path)) continue;

    const uint16_t index = validCount++;
    if (random(static_cast<long>(validCount)) == 0) any = {path, index, true};
    if (!APP_STATE.isRecentSleep(index, APP_STATE.recentSleepFill)) {
      ++freshCount;
      if (random(static_cast<long>(freshCount)) == 0) fresh = {path, index, true};
    }
  }
  dir.close();

  OverlayCandidate selected = fresh.valid ? std::move(fresh) : std::move(any);
  if (selected.valid) {
    APP_STATE.pushRecentSleep(selected.index);
    APP_STATE.saveToFile();
  }
  return selected;
}

OverlayCandidate rootOverlayCandidate(const char* path) {
  if (validateOverlayImage(path)) return OverlayCandidate{path, 0, true};
  return {};
}

OverlayCandidate directoryOverlayCandidate(const char* directoryPath) {
  return directoryImageCandidate(directoryPath, [](const std::string& path) { return validateOverlayImage(path); });
}

OverlayCandidate findTransparentSleepOverlay() {
  if (OverlayCandidate candidate = rootOverlayCandidate("/sleep-overlay.bmp"); candidate.valid) return candidate;
  if (OverlayCandidate candidate = rootOverlayCandidate("/sleep-overlay.png"); candidate.valid) return candidate;
  if (OverlayCandidate candidate = directoryOverlayCandidate("/.sleep-overlay"); candidate.valid) return candidate;
  return directoryOverlayCandidate("/sleep-overlay");
}

bool renderPngOverlay(const std::string& path, GfxRenderer& renderer) {
  ImageDimensions dimensions{};
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;
  const SleepImagePlacement placement = placeSleepImage(renderer, dimensions.width, dimensions.height);
  if (placement.width <= 0 || placement.height <= 0) return false;

  RenderConfig config{};
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = placement.width;
  config.maxHeight = placement.height;
  config.useDithering = true;
  config.useExactDimensions = true;
  config.preserveAlpha = true;
  config.writeWhiteInBw = true;
  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(path, renderer, config);
}

bool renderBitmapWhiteKeyOverlay(Bitmap& bitmap, GfxRenderer& renderer) {
  const SleepImagePlacement placement = placeSleepImage(renderer, bitmap.getWidth(), bitmap.getHeight());
  if (placement.width <= 0 || placement.height <= 0) return false;

  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto outputRow = makeUniqueNoThrow<uint8_t[]>(outputRowSize);
  auto rowBytes = makeUniqueNoThrow<uint8_t[]>(bitmap.getRowBytes());
  if (!outputRow || !rowBytes) return false;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); ++bmpY) {
    if (bitmap.readNextRow(outputRow.get(), rowBytes.get()) != BmpReaderError::Ok) return false;
    const int sourceY = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    const int firstY = placement.y +
                       static_cast<int>(static_cast<int64_t>(sourceY) * placement.height / bitmap.getHeight());
    const int endY = placement.y +
                     static_cast<int>(static_cast<int64_t>(sourceY + 1) * placement.height / bitmap.getHeight());
    if (firstY >= endY) continue;

    for (int bmpX = 0; bmpX < bitmap.getWidth(); ++bmpX) {
      const uint8_t value = (outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8))) & 0x03;
      if (value >= 3) continue;
      const int firstX = placement.x +
                         static_cast<int>(static_cast<int64_t>(bmpX) * placement.width / bitmap.getWidth());
      const int endX = placement.x +
                       static_cast<int>(static_cast<int64_t>(bmpX + 1) * placement.width / bitmap.getWidth());
      if (firstX >= endX) continue;
      const int clippedX = std::max(0, firstX);
      const int clippedY = std::max(0, firstY);
      const int clippedRight = std::min(renderer.getScreenWidth(), endX);
      const int clippedBottom = std::min(renderer.getScreenHeight(), endY);
      if (clippedX < clippedRight && clippedY < clippedBottom) {
        renderer.fillRect(clippedX, clippedY, clippedRight - clippedX, clippedBottom - clippedY);
      }
    }
  }
  return true;
}

bool renderBmpOverlay(const std::string& path, GfxRenderer& renderer) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;

  Bmp32OverlayHeader alphaHeader;
  const Bmp32HeaderStatus alphaStatus = readBmp32OverlayHeader(file, alphaHeader);
  if (alphaStatus == Bmp32HeaderStatus::Valid) {
    const bool rendered = renderBmp32Overlay(file, alphaHeader, renderer);
    file.close();
    return rendered;
  }
  if (alphaStatus == Bmp32HeaderStatus::Invalid) return file.close() && false;

  if (!file.seek(0)) return file.close() && false;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return file.close() && false;
  const bool rendered = renderBitmapWhiteKeyOverlay(bitmap, renderer);
  file.close();
  return rendered;
}

bool renderOverlayImage(const std::string& path, GfxRenderer& renderer) {
  if (FsHelpers::hasPngExtension(path)) return renderPngOverlay(path, renderer);
  if (FsHelpers::hasBmpExtension(path)) return renderBmpOverlay(path, renderer);
  return false;
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();
  wakeFrameReplayable_ = true;

  const bool renderQuickResume =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  LOG_INF("SLW", "sleep-activity enter screen=%u quick_resume=%u from_reader=%u",
          static_cast<unsigned>(SETTINGS.sleepScreen), static_cast<unsigned>(renderQuickResume),
          static_cast<unsigned>(APP_STATE.lastSleepFromReader));

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  const bool renderTransparent = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
  const bool transparentBaseSaved = renderTransparent && SleepFrameStore::save(renderer);
  const char* popupMessage = tr(STR_ENTERING_SLEEP);

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    showEnteringSleepPopup(renderer, popupMessage, renderTransparent);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    showEnteringSleepPopup(renderer, popupMessage, renderTransparent);
  }

  if (renderTransparent) return renderTransparentSleepScreen(transparentBaseSaved);

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_STATS):
      return renderCoverSleepScreen(true);
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM_STATS):
      return renderCustomSleepScreen(true);
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::READING_CALENDAR):
      return renderReadingCalendarSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderReadingCalendarSleepScreen() {
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats stats = GlobalReadingStats::load(&globalStatus);
  ReadingStatsDateTime now{};
  const bool clockValid = getCurrentLocalReadingStatsDateTime(now);
  if (!GlobalReadingStats::isTrustedLoadStatus(globalStatus) || !clockValid) {
    LOG_ERR("SLP", "Reading calendar unavailable: stats=%u clock=%d", static_cast<unsigned>(globalStatus), clockValid);
    return renderDefaultSleepScreen();
  }

  ReadingCalendarModel model(buildReadingCalendarSnapshot(stats, true, &now.date));
  DailyReadingHistory dailyHistory;
  const DailyReadingHistory::LoadStatus dailyStatus = DailyReadingHistory::load(dailyHistory);
  const bool dailyHistoryUsable = dailyStatus == DailyReadingHistory::LoadStatus::Ok ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::Missing ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
  if (!model.isAvailable() || !dailyHistoryUsable) {
    LOG_ERR("SLP", "Reading calendar unavailable: daily=%u", static_cast<unsigned>(dailyStatus));
    return renderDefaultSleepScreen();
  }
  if (model.snapshot().hasLatestDayReadingSeconds) {
    dailyHistory.reconcileExactDay(model.snapshot().latestReadingDay, model.snapshot().latestDayReadingSeconds);
  }
  model.setDailyHistory(&dailyHistory);

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const int side = std::max(18, metrics.contentSidePadding);
  const Rect content{screen.x + side, screen.y + 20, std::max(1, screen.width - side * 2),
                     std::max(1, screen.height - 40)};

  char month[16];
  snprintf(month, sizeof(month), "%02u/%04u", static_cast<unsigned>(model.visibleMonth().month),
           static_cast<unsigned>(model.visibleMonth().year));
  const int monthLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int monthWidth = renderer.getTextWidth(UI_10_FONT_ID, month, EpdFontFamily::BOLD) + 18;
  const int headerHeight = std::max(renderer.getLineHeight(UI_12_FONT_ID), monthLineHeight + 10);
  const int monthX = content.x + content.width - monthWidth;
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, tr(STR_READING_STATS),
                                                   std::max(1, content.width - monthWidth - 12), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, content.x,
                    content.y + std::max(0, (headerHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2), title.c_str(),
                    true, EpdFontFamily::BOLD);
  renderer.drawRoundedRect(monthX, content.y, monthWidth, headerHeight, 1, 5, true, true, true, true, true);
  drawCenteredInRect(renderer, UI_10_FONT_ID, Rect{monthX, content.y, monthWidth, headerHeight},
                     content.y + std::max(0, (headerHeight - monthLineHeight) / 2), month, EpdFontFamily::BOLD);

  const int titleBottom = content.y + headerHeight + 8;
  renderer.drawLine(content.x, titleBottom, content.x + content.width - 1, titleBottom);

  const ReadingCalendarMonthSummary summary = model.monthSummary();
  constexpr int sectionGap = 10;
  const int metricTop = titleBottom + sectionGap;
  const int weekdayHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const ReadingCalendarGridLayout naturalGrid =
      ReadingCalendarGridLayout::calculate(Rect{0, 0, content.width, content.height}, weekdayHeight);
  const int calendarHeight = weekdayHeight + naturalGrid.grid.height;
  const int legendHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int availableMetricHeight =
      std::max(1, content.y + content.height - metricTop - calendarHeight - legendHeight - sectionGap * 3);
  int primaryMetricHeight = std::clamp(availableMetricHeight * 3 / 5, 104, 152);
  int secondaryMetricHeight = availableMetricHeight - primaryMetricHeight;
  if (secondaryMetricHeight < 58) {
    secondaryMetricHeight = std::min(58, availableMetricHeight);
    primaryMetricHeight = std::max(1, availableMetricHeight - secondaryMetricHeight);
  }

  const Rect primaryMetrics{content.x, metricTop, content.width, primaryMetricHeight};
  renderer.drawRoundedRect(primaryMetrics.x, primaryMetrics.y, primaryMetrics.width, primaryMetrics.height, 1, 6, true,
                           true, true, true, true);
  const int totalWidth = primaryMetrics.width * 54 / 100;
  const int rightHeight = primaryMetrics.height / 2;
  renderer.drawLine(primaryMetrics.x + totalWidth, primaryMetrics.y + 5, primaryMetrics.x + totalWidth,
                    primaryMetrics.y + primaryMetrics.height - 6);
  renderer.drawLine(primaryMetrics.x + totalWidth + 5, primaryMetrics.y + rightHeight,
                    primaryMetrics.x + primaryMetrics.width - 6, primaryMetrics.y + rightHeight);

  char days[12];
  snprintf(days, sizeof(days), "%u", static_cast<unsigned>(summary.readingDays));
  char streak[12];
  snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(model.snapshot().currentStreak));
  drawCalendarMetric(renderer, Rect{primaryMetrics.x, primaryMetrics.y, totalWidth, primaryMetrics.height},
                     ReadingCalendarRenderer::formatDuration(summary.totalSeconds, summary.totalIsMinimum),
                     StrId::STR_STATS_MONTH_TOTAL, UI_12_FONT_ID);
  drawCalendarMetric(
      renderer, Rect{primaryMetrics.x + totalWidth, primaryMetrics.y, primaryMetrics.width - totalWidth, rightHeight},
      days, StrId::STR_STATS_MONTH_DAYS);
  drawCalendarMetric(renderer,
                     Rect{primaryMetrics.x + totalWidth, primaryMetrics.y + rightHeight,
                          primaryMetrics.width - totalWidth, primaryMetrics.height - rightHeight},
                     streak, StrId::STR_STATS_STREAK);

  const int calendarTop = primaryMetrics.y + primaryMetrics.height + sectionGap;
  const ReadingCalendarGridLayout calendarLayout =
      ReadingCalendarGridLayout::calculate(Rect{content.x, calendarTop, content.width, calendarHeight}, weekdayHeight);
  drawCalendarWeekdays(renderer, calendarLayout);
  const Rect grid = ReadingCalendarRenderer::drawGrid(renderer, calendarLayout.grid, model, false);

  const Rect secondaryMetrics{content.x, grid.y + grid.height + sectionGap, content.width, secondaryMetricHeight};
  renderer.drawRoundedRect(secondaryMetrics.x, secondaryMetrics.y, secondaryMetrics.width, secondaryMetrics.height, 1,
                           6, true, true, true, true, true);
  const int secondaryWidth = secondaryMetrics.width / 2;
  renderer.drawLine(secondaryMetrics.x + secondaryWidth, secondaryMetrics.y + 5, secondaryMetrics.x + secondaryWidth,
                    secondaryMetrics.y + secondaryMetrics.height - 6);
  char best[32] = "--";
  if (summary.bestDayKnown) {
    const std::string duration = ReadingCalendarRenderer::formatDuration(summary.bestDaySeconds);
    snprintf(best, sizeof(best), "%s · %u", duration.c_str(), static_cast<unsigned>(summary.bestDayOfMonth));
  }
  char longest[12];
  snprintf(longest, sizeof(longest), "%u", static_cast<unsigned>(summary.longestReadingStreak));
  drawCalendarMetric(renderer, Rect{secondaryMetrics.x, secondaryMetrics.y, secondaryWidth, secondaryMetrics.height},
                     best, StrId::STR_STATS_BEST_DAY);
  drawCalendarMetric(renderer,
                     Rect{secondaryMetrics.x + secondaryWidth, secondaryMetrics.y,
                          secondaryMetrics.width - secondaryWidth, secondaryMetrics.height},
                     longest, StrId::STR_STATS_LONGEST_STREAK);

  const int legendY = secondaryMetrics.y + secondaryMetrics.height + sectionGap;
  ReadingCalendarRenderer::drawLegend(renderer, Rect{grid.x, legendY, grid.width, legendHeight});

  displayStrongSleepFrame();
}

void SleepActivity::renderCustomSleepScreen(const bool withBookStats) {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap, false, withBookStats);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    dir.close();
    const OverlayCandidate selected =
        directoryImageCandidate(sleepDir, [](const std::string& path) { return validateBmpOverlay(path); });
    if (selected.valid) {
      HalFile randFile;
      if (Storage.openFileForRead("SLP", selected.path, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s", selected.path.c_str());
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap, false, withBookStats);
          randFile.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  if (withBookStats) {
    renderer.clearScreen();
    drawSleepBookStatsOverlay(renderer);
    displayStrongSleepFrame();
    return;
  }
  renderDefaultSleepScreen();
}

// Sleep is the last chance to remove accumulated charge before the panel is
// powered down. Use the strongest existing refresh here; normal UI and reader
// cadence remain unchanged.
void SleepActivity::renderDefaultSleepScreen() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const bool isX3 = gpio.deviceIsX3();
  const bool hasBundledScreen =
      (isX3 && pageWidth == DEFAULT_SLEEP_X3_WIDTH && pageHeight == DEFAULT_SLEEP_X3_HEIGHT) ||
      (!isX3 && pageWidth == DEFAULT_SLEEP_X4_WIDTH && pageHeight == DEFAULT_SLEEP_X4_HEIGHT);
  if (hasBundledScreen) {
    if (isX3) {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X3_WIDTH, DEFAULT_SLEEP_X3_HEIGHT, DefaultSleepX3Rows,
                               DefaultSleepX3Runs);
    } else {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X4_WIDTH, DEFAULT_SLEEP_X4_HEIGHT, DefaultSleepX4Rows,
                               DefaultSleepX4Runs);
    }
  } else {
    // Keep the generated logo fallback for an unexpected orientation or panel.
    renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));
  }

  displayStrongSleepFrame();
}

void SleepActivity::renderTransparentSleepScreen(const bool baseFrameSaved) {
  if (!baseFrameSaved) {
    LOG_ERR("SLP", "Transparent sleep skipped: base frame could not be saved");
    drawMoonOnCurrentFrame(renderer);
    displayStrongSleepFrame();
    return;
  }

  const OverlayCandidate overlay = findTransparentSleepOverlay();
  if (!overlay.valid) {
    LOG_DBG("SLP", "Transparent sleep overlay missing; using current frame");
    drawMoonOnCurrentFrame(renderer);
    displayStrongSleepFrame();
    return;
  }

  LOG_DBG("SLP", "Rendering transparent sleep overlay: %s", overlay.path.c_str());
  if (renderOverlayImage(overlay.path, renderer)) {
    displayStrongSleepFrame();
    return;
  }

  LOG_ERR("SLP", "Transparent sleep overlay failed: %s", overlay.path.c_str());
  if (SleepFrameStore::load(display, false)) {
    drawMoonOnCurrentFrame(renderer);
    displayStrongSleepFrame();
    return;
  }

  wakeFrameReplayable_ = false;
  renderDefaultSleepScreen();
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool applyCoverFilter,
                                            const bool withBookStats) {
  const SleepImagePlacement placement = placeSleepImage(renderer, bitmap.getWidth(), bitmap.getHeight());
  LOG_DBG("SLP", "bitmap %d x %d -> %d x %d at %d,%d zoom=%u", bitmap.getWidth(), bitmap.getHeight(),
          placement.width, placement.height, placement.x, placement.y,
          static_cast<unsigned>(SETTINGS.sleepScreenImageZoom));
  renderer.clearScreen();

  const uint8_t filter =
      applyCoverFilter ? SETTINGS.sleepScreenCoverFilter : CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  const bool hasGreyscale = bitmap.hasGreyscale() && renderer.supportsStripGrayscale() &&
                            filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);

  const SleepBookSummary statsSummary = withBookStats ? loadSleepBookSummary() : SleepBookSummary{};
  const Rect statsCard = withBookStats ? drawSleepBookStatsOverlay(renderer, statsSummary) : Rect{};

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  const unsigned long grayscaleStartedAt = hasGreyscale ? millis() : 0;
  if (hasGreyscale) {
    LOG_INF("SLW", "sleep-frame grayscale begin base=full turn_off=1");
    applySleepGhostingTreatment();
    prepareStrongSleepRefresh();
    renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
    LOG_INF("SLW", "sleep-frame grayscale base complete elapsed_ms=%lu", millis() - grayscaleStartedAt);
  } else {
    displayStrongSleepFrame();
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);
    if (statsCard.width > 0 && statsCard.height > 0) {
      renderer.fillRect(statsCard.x, statsCard.y, statsCard.width, statsCard.height, true);
    }
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);
    if (statsCard.width > 0 && statsCard.height > 0) {
      renderer.fillRect(statsCard.x, statsCard.y, statsCard.width, statsCard.height, true);
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    LOG_INF("SLW", "sleep-frame grayscale planes complete elapsed_ms=%lu", millis() - grayscaleStartedAt);
    renderer.setRenderMode(GfxRenderer::BW);

    // The panel keeps the grayscale image, but the shared framebuffer now
    // contains only the last gray plane. Rebuild the same composition as B/W
    // without touching the powered-down panel so normal wake can replay it.
    const bool rewound = bitmap.rewindToData() == BmpReaderError::Ok;
    renderer.clearScreen();
    const bool coverReady = rewound && renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width,
                                                           placement.height, 0, 0, true);
    if (withBookStats) drawSleepBookStatsOverlay(renderer, statsSummary);
    if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    wakeFrameReplayable_ = coverReady;
    LOG_INF("SLW", "sleep-frame bw surrogate ready=%u", static_cast<unsigned>(wakeFrameReplayable_));
  }
}

void SleepActivity::renderCoverSleepScreen(const bool withBookStats) {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;
  constexpr bool cropped = false;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return renderDefaultSleepScreen();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath) ||
             FsHelpers::hasMarkdownExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return renderDefaultSleepScreen();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
    const bool cachedCoverReady = Bitmap::inspectDerivedCache(coverBmpPath) == BitmapCacheState::Ready &&
                                  lastEpub.inspectSourceBinding() == Epub::SourceBindingStatus::Match;
    if (!cachedCoverReady) {
      // Skip loading CSS since we only need metadata when the validated
      // full-screen derived cover is absent.
      if (!lastEpub.load(true, true)) {
        LOG_ERR("SLP", "Failed to load last epub");
        return renderDefaultSleepScreen();
      }

      const Epub::ThumbnailRequest thumbnails{
          CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView),
          CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout),
          renderer.getDisplayHeight() == 528,
      };
      if (!lastEpub.generateCoverBmp(cropped, thumbnails)) {
        LOG_ERR("SLP", "Failed to generate cover bmp");
        return renderDefaultSleepScreen();
      }
    }
  } else {
    return renderDefaultSleepScreen();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap, true, withBookStats);
      return;
    }
  }

  return renderDefaultSleepScreen();
}

void SleepActivity::renderLastScreenSleepScreen() {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  displayStrongSleepFrame();
}

void SleepActivity::renderBlankSleepScreen() {
  renderer.clearScreen();
  displayStrongSleepFrame();
}
