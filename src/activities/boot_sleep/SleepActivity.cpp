#include "SleepActivity.h"

#include <Epub.h>
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
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/DailyReadingHistory.h"
#include "activities/reader/GlobalReadingStats.h"
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
// Keep one X3 conditioning pass for the strong sleep cleanup, without the
// previous second pass that could over-drive the parked image.
constexpr uint8_t X3_SLEEP_CONDITION_PASSES = 1;

void prepareStrongSleepRefresh() { display.requestResync(X3_SLEEP_CONDITION_PASSES); }

void displayStrongSleepFrame() {
  prepareStrongSleepRefresh();
  // Wait for the waveform and POWER_OFF to finish here. Persisting state after
  // an asynchronous trigger could otherwise leave the panel driven while a
  // slow SD write is still in progress, making the parked image darken.
  display.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
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
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
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

void SleepActivity::renderReadingCalendarSleepScreen() const {
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

void SleepActivity::renderCustomSleepScreen() const {
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
      renderBitmapSleepScreen(bitmap, false);
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
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        dirFile.close();
        continue;
      }
      Bitmap bitmap(dirFile);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        dirFile.close();
        continue;
      }
      files.emplace_back(filename);
      dirFile.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      HalFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap, false);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

// Sleep is the last chance to remove accumulated charge before the panel is
// powered down. Use the strongest existing refresh here; normal UI and reader
// cadence remain unchanged.
void SleepActivity::renderDefaultSleepScreen() const {
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

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool applyCoverSettings) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (applyCoverSettings && SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (applyCoverSettings && SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const uint8_t filter =
      applyCoverSettings ? SETTINGS.sleepScreenCoverFilter : CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  const bool hasGreyscale = bitmap.hasGreyscale() && renderer.supportsStripGrayscale() &&
                            filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    prepareStrongSleepRefresh();
    renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
  } else {
    displayStrongSleepFrame();
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

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
      renderBitmapSleepScreen(bitmap, true);
      return;
    }
  }

  return renderDefaultSleepScreen();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  displayStrongSleepFrame();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  displayStrongSleepFrame();
}
