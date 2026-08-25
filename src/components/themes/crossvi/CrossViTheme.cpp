#include "CrossViTheme.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/home/HomeBookSummary.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/medal.h"
#include "components/icons/pin.h"
#include "components/icons/recent.h"
#include "components/icons/search24.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "components/themes/HomeMenuLayout.h"
#include "components/themes/crossvi/CrossViLayout.h"
#include "fontIds.h"

namespace {

constexpr int CORNER_RADIUS = 6;
constexpr int MENU_COLUMNS = 2;
constexpr int MENU_GAP = 8;
constexpr int MENU_SIDE_PADDING = 20;
constexpr int MENU_ICON_SIZE = 32;
constexpr int MENU_CONTENT_PADDING = 18;
constexpr int H_PADDING_IN_SELECTION = 8;
constexpr int TOP_HINT_BUTTON_Y = 345;
constexpr int MAX_LIST_VALUE_WIDTH = 200;
constexpr int LIST_ICON_SIZE = 24;

const uint8_t* iconForName(const UIIcon icon, const int size = 32) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      case UIIcon::Search:
        return Search24Icon;
      default:
        return nullptr;
    }
  }
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    case UIIcon::Medal:
      return MedalIcon;
    default:
      return nullptr;
  }
}

std::string formatDuration(const uint32_t seconds) {
  char value[32]{};
  const uint32_t minutes = seconds == 0 ? 0 : std::max<uint32_t>(1, seconds / 60);
  snprintf(value, sizeof(value), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned>(minutes));
  return seconds > 0 && seconds < 60 ? "<" + std::string(value) : std::string(value);
}

std::string formatHomeReadingTime(const uint32_t seconds) {
  const std::string duration = formatDuration(seconds);
  char value[96]{};
  snprintf(value, sizeof(value), tr(STR_HOME_READING_TIME_FORMAT), duration.c_str());
  return value;
}

StrId homeBookActionLabelId(const HomeBookSummary& summary) {
  switch (homeBookAction(summary)) {
    case HomeBookAction::Continue:
      return StrId::STR_CONTINUE_READING;
    case HomeBookAction::ReadAgain:
      return StrId::STR_READ_AGAIN;
    case HomeBookAction::Start:
      return StrId::STR_START_BOOK;
    case HomeBookAction::Open:
    default:
      return StrId::STR_OPEN_BOOK;
  }
}

std::string displayTitleForBook(const RecentBook& book) {
  std::string title = book.title.empty() ? book.path : book.title;
  const size_t lastSlash = title.find_last_of('/');
  if (lastSlash != std::string::npos) title.erase(0, lastSlash + 1);

  size_t extensionLength = 0;
  if (FsHelpers::hasEpubExtension(title) || FsHelpers::checkFileExtension(title, ".xtch")) {
    extensionLength = 5;
  } else if (FsHelpers::hasTxtExtension(title) || FsHelpers::checkFileExtension(title, ".xtc") ||
             FsHelpers::checkFileExtension(title, ".pdf")) {
    extensionLength = 4;
  } else if (FsHelpers::hasMarkdownExtension(title)) {
    extensionLength = 3;
  }
  if (extensionLength > 0) title.resize(title.size() - extensionLength);

  std::replace(title.begin(), title.end(), '_', ' ');
  title.erase(std::unique(title.begin(), title.end(),
                          [](const char left, const char right) { return left == ' ' && right == ' '; }),
              title.end());
  return title;
}

const char* recentBookFormatLabel(const RecentBook& book) {
  if (FsHelpers::hasTxtExtension(book.path)) return "TXT";
  if (FsHelpers::hasMarkdownExtension(book.path)) return "Markdown";
  if (FsHelpers::checkFileExtension(book.path, ".xtch")) return "XTCH";
  if (FsHelpers::checkFileExtension(book.path, ".xtc")) return "XTC";
  return "EPUB";
}

void drawMenuIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size,
                  const bool black) {
  if (black) {
    renderer.drawIcon(bitmap, x, y, size);
    return;
  }

  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      const uint8_t byte = bitmap[row * rowBytes + (column >> 3)];
      if (((byte >> (7 - (column & 7))) & 1) == 0) renderer.drawPixel(x + size - 1 - row, y + column, false);
    }
  }
}

void drawStatsIcon(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawRoundedRect(x, y, MENU_ICON_SIZE, MENU_ICON_SIZE, 1, 3, true, true, true, true, black);
  renderer.fillRect(x + 6, y + 19, 4, 7, black);
  renderer.fillRect(x + 14, y + 13, 4, 13, black);
  renderer.fillRect(x + 22, y + 7, 4, 19, black);
}

void drawRecentHistoryIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int SIZE = 28;
  constexpr int INSET = 3;
  constexpr int DIAMETER = SIZE - INSET * 2;
  renderer.drawRoundedRect(x + INSET, y + INSET, DIAMETER, DIAMETER, 2, DIAMETER / 2, true, true, true, true, true);
  const int centerX = x + SIZE / 2;
  const int centerY = y + SIZE / 2;
  renderer.drawLine(centerX, centerY - 6, centerX, centerY, 2, true);
  renderer.drawLine(centerX, centerY, centerX + 5, centerY + 3, 2, true);
}

void drawBookmarkIcon(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawLine(x + 7, y + 5, x + 24, y + 5, black);
  renderer.drawLine(x + 7, y + 6, x + 24, y + 6, black);
  renderer.drawLine(x + 7, y + 5, x + 7, y + 27, black);
  renderer.drawLine(x + 8, y + 5, x + 8, y + 27, black);
  renderer.drawLine(x + 23, y + 5, x + 23, y + 27, black);
  renderer.drawLine(x + 24, y + 5, x + 24, y + 27, black);
  renderer.drawLine(x + 7, y + 27, x + 15, y + 21, black);
  renderer.drawLine(x + 8, y + 27, x + 15, y + 22, black);
  renderer.drawLine(x + 15, y + 21, x + 24, y + 27, black);
  renderer.drawLine(x + 15, y + 22, x + 23, y + 27, black);
}

constexpr int CAROUSEL_DOT_SIZE = 14;

void drawCarouselDot(const GfxRenderer& renderer, const int x, const int y, const bool selected) {
  const int size = selected ? CAROUSEL_DOT_SIZE : CAROUSEL_DOT_SIZE - 4;
  const int inset = (CAROUSEL_DOT_SIZE - size) / 2;
  const int outerRadiusSquared = (size - 1) * (size - 1);
  const int innerRadiusSquared = (size - 5) * (size - 5);
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      const int dx = column * 2 - (size - 1);
      const int dy = row * 2 - (size - 1);
      const int distanceSquared = dx * dx + dy * dy;
      if (distanceSquared <= outerRadiusSquared && (selected || distanceSquared >= innerRadiusSquared)) {
        renderer.drawPixel(x + inset + column, y + inset + row);
      }
    }
  }
}

bool finalLineIsEllipsized(const std::vector<std::string>& lines) {
  constexpr char ELLIPSIS[] = "\xE2\x80\xA6";
  return !lines.empty() && lines.back().size() >= sizeof(ELLIPSIS) - 1 &&
         lines.back().compare(lines.back().size() - (sizeof(ELLIPSIS) - 1), sizeof(ELLIPSIS) - 1, ELLIPSIS) == 0;
}

void drawCenteredText(const GfxRenderer& renderer, const int fontId, const Rect rect, const char* text,
                      const bool black = true, const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                      const int measuredWidth = -1) {
  const int textWidth = measuredWidth >= 0 ? measuredWidth : renderer.getTextWidth(fontId, text, style);
  const int lineHeight = renderer.getLineHeight(fontId);
  renderer.drawText(fontId, rect.x + std::max(0, (rect.width - textWidth) / 2),
                    rect.y + std::max(0, (rect.height - lineHeight) / 2), text, black, style);
}

Rect topRightBatteryRect(const Rect rect) {
  return Rect{rect.x + rect.width - CrossViMetrics::values.contentSidePadding - CrossViMetrics::values.batteryWidth,
              rect.y + 8, CrossViMetrics::values.batteryWidth, CrossViMetrics::values.batteryHeight};
}

bool outsideDateTimeText(char (&value)[40]) {
  if (!halClock.isAvailable()) return false;

  const bool showTime = SETTINGS.outsideReaderClock != 0;
  const bool showDate = SETTINGS.showDateOutsideReader;
  if (!showTime && !showDate) return false;

  char time[9]{};
  const bool haveTime =
      showTime && halClock.formatTime(time, sizeof(time), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);
  if (!showDate) {
    if (!haveTime) return false;
    std::snprintf(value, sizeof(value), "%s", time);
    return true;
  }

  char separator = '/';
  if (SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_PERIOD) {
    separator = '.';
  } else if (SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_HYPHEN) {
    separator = '-';
  }
  char date[24]{};
  const bool haveDate = halClock.formatDate(date, sizeof(date), SETTINGS.clockUtcOffsetQ,
                                            static_cast<HalClock::DateFormat>(SETTINGS.dateFormat), separator,
                                            I18N.getLanguage() == Language::VI);
  if (!haveDate && !haveTime) return false;
  if (!haveDate)
    std::snprintf(value, sizeof(value), "%s", time);
  else if (!haveTime)
    std::snprintf(value, sizeof(value), "%s", date);
  else if (SETTINGS.outsideReaderDateTimeOrder == CrossPointSettings::OUTSIDE_READER_TIME_THEN_DATE)
    std::snprintf(value, sizeof(value), "%s %s", time, date);
  else
    std::snprintf(value, sizeof(value), "%s %s", date, time);
  return true;
}

void drawOutsideClockRight(const GfxRenderer& renderer, const Rect rect, const int batteryLeft, const char* value) {
  const int width = renderer.getTextWidth(SMALL_FONT_ID, value);
  const int x = batteryLeft - 10 - width;
  renderer.drawText(SMALL_FONT_ID, std::max(rect.x + CrossViMetrics::values.contentSidePadding, x), rect.y + 8, value);
}

void drawPinStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int size = 16;
  const int iconX = x + (LIST_ICON_SIZE - size) / 2;
  const int iconY = y + (LIST_ICON_SIZE - size) / 2;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const uint8_t byte = PinStatusIcon[row * 2 + (col >> 3)];
      if ((byte & (1U << (7 - (col & 7)))) == 0) renderer.drawPixel(iconX + col, iconY + row);
    }
  }
}

// Prefer the thumbnail made for this UI, then the shared and legacy sizes.
// Rendering never regenerates a cover while holding the render lock.
bool openExistingCover(const std::string& pattern, const int requestedHeight, const char* module, HalFile& file) {
  const int candidates[] = {requestedHeight, 240, 168};
  int previous = -1;
  for (const int height : candidates) {
    if (height <= 0 || height == previous) continue;
    previous = height;
    const std::string path = UITheme::getCoverThumbPath(pattern, height);
    if (Storage.openFileForRead(module, path, file)) return true;
  }
  return false;
}

bool drawCarouselBitmap(const GfxRenderer& renderer, Bitmap& bitmap, const Rect frame, const bool centerCrop,
                        const int perspectiveDirection, Rect* renderedBounds = nullptr) {
  if (!bitmap.is1Bit() || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || frame.width <= 0 || frame.height <= 0) {
    return false;
  }

  constexpr size_t MAX_THUMBNAIL_BYTES = 64 * 1024;
  const uint64_t packedBytes64 = static_cast<uint64_t>(bitmap.getRowBytes()) * bitmap.getHeight();
  if (packedBytes64 == 0 || packedBytes64 > MAX_THUMBNAIL_BYTES || packedBytes64 > std::numeric_limits<size_t>::max()) {
    return false;
  }

  const size_t packedBytes = static_cast<size_t>(packedBytes64);
  auto* rows = static_cast<uint8_t*>(malloc(packedBytes));
  if (!rows) return false;
  if (bitmap.readPackedRows(rows, packedBytes) != BmpReaderError::Ok) {
    free(rows);
    return false;
  }

  const int sourceWidth = bitmap.getWidth();
  const int sourceHeight = bitmap.getHeight();
  int cropX = 0;
  int cropY = 0;
  int cropWidth = sourceWidth;
  int cropHeight = sourceHeight;
  if (centerCrop) {
    if (static_cast<int64_t>(sourceWidth) * frame.height > static_cast<int64_t>(sourceHeight) * frame.width) {
      cropWidth = std::max(1, sourceHeight * frame.width / frame.height);
      cropX = (sourceWidth - cropWidth) / 2;
    } else {
      cropHeight = std::max(1, sourceWidth * frame.height / frame.width);
      cropY = (sourceHeight - cropHeight) / 2;
    }
  }

  const Rect drawBounds = centerCrop ? frame : CrossViCarouselLayout::fitCoverWithin(frame, cropWidth, cropHeight);
  const int drawWidth = drawBounds.width;
  const int drawHeight = drawBounds.height;
  const int drawX = drawBounds.x;
  const int drawY = drawBounds.y;
  if (renderedBounds) *renderedBounds = drawBounds;

  // Preserve the focus fill in the letterbox area. Clearing the nominal frame
  // punched white strips above/below covers whose aspect ratio did not fill it.
  renderer.fillRect(drawBounds.x, drawBounds.y, drawBounds.width, drawBounds.height, false);
  if (perspectiveDirection == 0) {
    const int firstX = std::max(0, -drawX);
    const int lastX = std::min(drawWidth, renderer.getScreenWidth() - drawX);
    const int firstY = std::max(0, -drawY);
    const int lastY = std::min(drawHeight, renderer.getScreenHeight() - drawY);
    for (int outputY = firstY; outputY < lastY; ++outputY) {
      const int sampledY = cropY + std::min(cropHeight - 1, outputY * cropHeight / drawHeight);
      const int sourceY = bitmap.isTopDown() ? sampledY : sourceHeight - 1 - sampledY;
      const uint8_t* sourceRow = rows + static_cast<size_t>(sourceY) * bitmap.getRowBytes();
      for (int outputX = firstX; outputX < lastX; ++outputX) {
        const int sourceX = cropX + std::min(cropWidth - 1, outputX * cropWidth / drawWidth);
        const uint8_t paletteIndex = (sourceRow[sourceX >> 3] >> (7 - (sourceX & 7))) & 1U;
        if (bitmap.paletteLuminance(paletteIndex) < 128) renderer.drawPixel(drawX + outputX, drawY + outputY);
      }
    }
  } else {
    const int firstX = std::max(0, -drawX);
    const int lastX = std::min(drawWidth, renderer.getScreenWidth() - drawX);
    for (int outputX = firstX; outputX < lastX; ++outputX) {
      const int inset = CrossViCarouselLayout::perspectiveColumnInset(perspectiveDirection, outputX, drawWidth);
      const int columnHeight = std::max(1, drawHeight - inset * 2);
      const int sourceX = cropX + std::min(cropWidth - 1, outputX * cropWidth / drawWidth);
      const int columnY = drawY + inset;
      const int firstY = std::max(0, -columnY);
      const int lastY = std::min(columnHeight, renderer.getScreenHeight() - columnY);
      const uint32_t sourceStep = static_cast<uint32_t>((static_cast<uint64_t>(cropHeight) << 16U) / columnHeight);
      for (int outputY = firstY; outputY < lastY; ++outputY) {
        const int sampledY = cropY + std::min(cropHeight - 1, static_cast<int>((outputY * sourceStep) >> 16U));
        const int sourceY = bitmap.isTopDown() ? sampledY : sourceHeight - 1 - sampledY;
        const uint8_t* sourceRow = rows + static_cast<size_t>(sourceY) * bitmap.getRowBytes();
        const uint8_t paletteIndex = (sourceRow[sourceX >> 3] >> (7 - (sourceX & 7))) & 1U;
        if (bitmap.paletteLuminance(paletteIndex) < 128) renderer.drawPixel(drawX + outputX, columnY + outputY);
      }
    }
  }
  free(rows);
  return true;
}

bool drawCarouselBookCover(const GfxRenderer& renderer, const RecentBook& book, const Rect frame, const bool selected,
                           const bool centerCrop, const int perspectiveDirection, const int thumbnailHeight,
                           const char* missingCoverText = nullptr) {
  Rect visibleFrame = frame;
  bool drewCover = false;
  if (!book.coverBmpPath.empty()) {
    HalFile file;
    if (openExistingCover(book.coverBmpPath, thumbnailHeight, "HOME", file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        drewCover = drawCarouselBitmap(renderer, bitmap, frame, centerCrop, perspectiveDirection, &visibleFrame);
      }
    }
  }

  if (!drewCover) {
    if (perspectiveDirection == 0) {
      renderer.fillRect(frame.x, frame.y, frame.width, frame.height, false);
      renderer.fillRectDither(frame.x, frame.y, frame.width, frame.height, Color::LightGray);
    } else {
      renderer.fillRect(frame.x, frame.y, frame.width, frame.height, false);
      for (int column = 0; column < frame.width; ++column) {
        const int inset = CrossViCarouselLayout::perspectiveColumnInset(perspectiveDirection, column, frame.width);
        renderer.fillRectDither(frame.x + column, frame.y + inset, 1, std::max(1, frame.height - inset * 2),
                                Color::LightGray);
      }
    }
    const int iconX = frame.x + (frame.width - MENU_ICON_SIZE) / 2;
    std::vector<std::string> messageLines;
    if (missingCoverText && perspectiveDirection == 0) {
      messageLines = renderer.wrappedText(SMALL_FONT_ID, missingCoverText, std::max(0, frame.width - 32), 3);
    }
    const int messageLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int messageHeight = static_cast<int>(messageLines.size()) * messageLineHeight;
    const int contentHeight = MENU_ICON_SIZE + (messageLines.empty() ? 0 : 12 + messageHeight);
    const int iconY = frame.y + std::max(0, (frame.height - contentHeight) / 2);
    if (iconX + MENU_ICON_SIZE > 0 && iconX < renderer.getScreenWidth()) {
      renderer.drawIcon(CoverIcon, iconX, iconY, MENU_ICON_SIZE);
    }
    int messageY = iconY + MENU_ICON_SIZE + 12;
    for (const std::string& line : messageLines) {
      const int width = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      renderer.drawText(SMALL_FONT_ID, frame.x + std::max(0, (frame.width - width) / 2), messageY, line.c_str());
      messageY += messageLineHeight;
    }
  }

  const bool fullyVisible = visibleFrame.x >= 0 && visibleFrame.y >= 0 &&
                            visibleFrame.x + visibleFrame.width <= renderer.getScreenWidth() &&
                            visibleFrame.y + visibleFrame.height <= renderer.getScreenHeight();
  if (fullyVisible && !centerCrop) {
    renderer.drawRect(visibleFrame.x, visibleFrame.y, visibleFrame.width, visibleFrame.height, true);
    if (selected && visibleFrame.width > 2 && visibleFrame.height > 2) {
      renderer.drawRect(visibleFrame.x + 1, visibleFrame.y + 1, visibleFrame.width - 2, visibleFrame.height - 2, true);
    }
  } else if (centerCrop && perspectiveDirection != 0) {
    const int leftInset = CrossViCarouselLayout::perspectiveColumnInset(perspectiveDirection, 0, frame.width);
    const int rightInset =
        CrossViCarouselLayout::perspectiveColumnInset(perspectiveDirection, frame.width - 1, frame.width);
    renderer.drawLine(frame.x, frame.y + leftInset, frame.x + frame.width - 1, frame.y + rightInset, true);
    renderer.drawLine(frame.x, frame.y + frame.height - 1 - leftInset, frame.x + frame.width - 1,
                      frame.y + frame.height - 1 - rightInset, true);
    renderer.drawLine(frame.x, frame.y + leftInset, frame.x, frame.y + frame.height - 1 - leftInset, true);
    renderer.drawLine(frame.x + frame.width - 1, frame.y + rightInset, frame.x + frame.width - 1,
                      frame.y + frame.height - 1 - rightInset, true);
  } else if (centerCrop) {
    renderer.drawRect(frame.x, frame.y, frame.width, frame.height, true);
  } else {
    const int edgeX = frame.x < 0 ? frame.x + frame.width - 1 : frame.x;
    const int top = std::max(0, frame.y);
    const int bottom = std::min(renderer.getScreenHeight() - 1, frame.y + frame.height - 1);
    if (edgeX >= 0 && edgeX < renderer.getScreenWidth() && bottom >= top) {
      renderer.drawLine(edgeX, top, edgeX, bottom, true);
    }
  }
  return drewCover;
}

}  // namespace

void CrossViTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  if (gpio.isUsbConnected()) {
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
    return;
  }
  if (percentage > 10) renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
  if (percentage > 40) renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
  if (percentage > 70) renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
}

void CrossViTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t batteryPercentage = powerManager.getBatteryPercentage();
  const Rect battery = topRightBatteryRect(rect);
  const int batteryLeft = drawBatteryRight(renderer, battery, batteryPercentage, showBatteryPercentage);
  char timeValue[40]{};
  if (outsideDateTimeText(timeValue)) {
    drawOutsideClockRight(renderer, rect, batteryLeft, timeValue);
  }

  int titleWidth = title ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int subtitleWidth = subtitle ? renderer.getTextWidth(SMALL_FONT_ID, subtitle) : 0;
  const int available = rect.width - CrossViMetrics::values.contentSidePadding * 3;
  if (titleWidth + subtitleWidth > available) {
    if (titleWidth > available / 2 && subtitleWidth > available / 2) {
      titleWidth = available / 2;
      subtitleWidth = available / 2;
    } else if (titleWidth > subtitleWidth) {
      titleWidth = available - subtitleWidth;
    } else {
      subtitleWidth = available - titleWidth;
    }
  }
  if (title) {
    const auto text = renderer.truncatedText(UI_12_FONT_ID, title, titleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + CrossViMetrics::values.contentSidePadding,
                      rect.y + CrossViMetrics::values.batteryBarHeight + 3, text.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }
  if (subtitle) {
    const auto text = renderer.truncatedText(SMALL_FONT_ID, subtitle, subtitleWidth);
    const int width = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - CrossViMetrics::values.contentSidePadding - width,
                      rect.y + 50, text.c_str());
  }
}

void CrossViTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                                 const char* rightLabel) const {
  int rightSpace = CrossViMetrics::values.contentSidePadding;
  if (rightLabel) {
    const auto right = renderer.truncatedText(SMALL_FONT_ID, rightLabel, MAX_LIST_VALUE_WIDTH);
    const int width = renderer.getTextWidth(SMALL_FONT_ID, right.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - CrossViMetrics::values.contentSidePadding - width,
                      rect.y + 7, right.c_str());
    rightSpace += width + H_PADDING_IN_SELECTION;
  }
  const auto text =
      renderer.truncatedText(UI_10_FONT_ID, label, rect.width - CrossViMetrics::values.contentSidePadding - rightSpace);
  renderer.drawText(UI_10_FONT_ID, rect.x + CrossViMetrics::values.contentSidePadding, rect.y + 6, text.c_str());
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void CrossViTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::span<const TabInfo> tabs,
                              bool selected) const {
  int currentX = rect.x + CrossViMetrics::values.contentSidePadding;
  for (const auto& tab : tabs) {
    const int width = renderer.getTextWidth(UI_10_FONT_ID, tab.label);
    if (tab.selected) {
      if (selected) {
        renderer.fillRectDither(currentX, rect.y, width + 2 * H_PADDING_IN_SELECTION, rect.height - 3,
                                Color::LightGray);
      }
      renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + width + 2 * H_PADDING_IN_SELECTION,
                        rect.y + rect.height - 3, 2, true);
    }
    renderer.drawText(UI_10_FONT_ID, currentX + H_PADDING_IN_SELECTION, rect.y + 6, tab.label);
    currentX += width + CrossViMetrics::values.tabSpacing + 2 * H_PADDING_IN_SELECTION;
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

int CrossViTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  return contentHeight /
         (hasSubtitle ? CrossViMetrics::values.listWithSubtitleRowHeight : CrossViMetrics::values.listRowHeight);
}

void CrossViTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed,
                            const std::function<bool(int index)>& rowBadge, const int pageAnchorIndex,
                            const std::function<int(int index)>& rowValueReservedWidth) const {
  const int rowHeight =
      rowSubtitle ? CrossViMetrics::values.listWithSubtitleRowHeight : CrossViMetrics::values.listRowHeight;
  const int pageItems = rect.height / rowHeight;
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollBarHeight = (rect.height * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((rect.height - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - CrossViMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + rect.height, true);
    renderer.fillRect(scrollBarX - CrossViMetrics::values.scrollBarWidth, scrollBarY,
                      CrossViMetrics::values.scrollBarWidth, scrollBarHeight, true);
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? CrossViMetrics::values.scrollBarWidth + CrossViMetrics::values.scrollBarRightOffset : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(
        rect.x + CrossViMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
        contentWidth - CrossViMetrics::values.contentSidePadding * 2, rowHeight, CORNER_RADIUS, Color::LightGray);
  }

  int textX = rect.x + CrossViMetrics::values.contentSidePadding + H_PADDING_IN_SELECTION;
  int textWidth = contentWidth - CrossViMetrics::values.contentSidePadding * 2 - H_PADDING_IN_SELECTION * 2;
  const int iconSize = rowSubtitle ? MENU_ICON_SIZE : LIST_ICON_SIZE;
  if (rowIcon) {
    textX += iconSize + H_PADDING_IN_SELECTION;
    textWidth -= iconSize + H_PADDING_IN_SELECTION;
  }

  const int pageIndex = pageAnchorIndex >= 0 ? pageAnchorIndex : selectedIndex;
  const int pageStart = pageIndex < 0 ? 0 : pageIndex / pageItems * pageItems;
  const int iconY = rowSubtitle ? 16 : 10;
  for (int i = pageStart; i < itemCount && i < pageStart + pageItems; ++i) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;
    std::string value;
    int valueWidth = 0;
    if (rowValue) {
      int measuredValueWidth = 0;
      value = renderer.truncatedText(UI_10_FONT_ID, rowValue(i).c_str(), MAX_LIST_VALUE_WIDTH, EpdFontFamily::REGULAR,
                                     &measuredValueWidth);
      valueWidth = measuredValueWidth + H_PADDING_IN_SELECTION;
    }
    if (rowValueReservedWidth) valueWidth = std::max(valueWidth, rowValueReservedWidth(i));
    rowTextWidth = std::max(0, rowTextWidth - valueWidth);
    const bool hasBadge = rowBadge && rowBadge(i);
    if (hasBadge) {
      rowTextWidth = std::max(0, rowTextWidth - LIST_ICON_SIZE - H_PADDING_IN_SELECTION);
    }

    int titleWidth = 0;
    const auto title =
        renderer.truncatedText(UI_10_FONT_ID, rowTitle(i).c_str(), rowTextWidth, EpdFontFamily::REGULAR, &titleWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, title.c_str());
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int height = renderer.getLineHeight(UI_10_FONT_ID);
      for (int y = itemY + 7; y < itemY + 7 + height; ++y)
        for (int x = textX; x < textX + titleWidth; ++x)
          if ((x + y) % 2 == 0) renderer.drawPixel(x, y, false);
    }
    if (rowIcon) {
      const UIIcon icon = rowIcon(i);
      const int iconX = rect.x + CrossViMetrics::values.contentSidePadding + H_PADDING_IN_SELECTION;
      if (icon == UIIcon::Bookmark && iconSize == MENU_ICON_SIZE) {
        drawBookmarkIcon(renderer, iconX, itemY + iconY, true);
      } else if (icon == UIIcon::Text && iconSize == MENU_ICON_SIZE) {
        // Subtitle rows reserve a 32px icon lane.  The text-file icon is
        // 24px, so center it rather than treating it as a 32px bitmap.
        renderer.drawIcon(Text24Icon, iconX + 4, itemY + iconY + 4, LIST_ICON_SIZE);
      } else if (const uint8_t* bitmap = iconForName(icon, iconSize)) {
        renderer.drawIcon(bitmap, iconX, itemY + iconY, iconSize);
      }
    }
    if (rowSubtitle) {
      const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, rowSubtitle(i).c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str());
    }
    if (!value.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - CrossViMetrics::values.contentSidePadding - H_PADDING_IN_SELECTION - valueWidth,
            itemY, valueWidth + H_PADDING_IN_SELECTION, rowHeight, CORNER_RADIUS, Color::Black);
      }
      const int valueY = itemY + (rowSubtitle ? 16 : 6);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - CrossViMetrics::values.contentSidePadding - valueWidth,
                        valueY, value.c_str(), !(i == selectedIndex && highlightValue));
    }
    if (hasBadge) {
      drawPinStatusIcon(renderer, rect.x + contentWidth - CrossViMetrics::values.contentSidePadding - LIST_ICON_SIZE,
                        itemY + 4);
    }
  }
}

void CrossViTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  const auto orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int textYOffset = 7;
  constexpr int x4Positions[] = {58, 146, 254, 342};
  constexpr int x3Positions[] = {65, 157, 291, 383};
  const int* positions = gpio.deviceIsX3() ? x3Positions : x4Positions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int height = renderer.getScreenHeight();
  for (int i = 0; i < 4; ++i) {
    const int y =
        labels[i] && labels[i][0] ? height - CrossViMetrics::values.buttonHintsHeight : height - smallButtonHeight;
    const int buttonHeight = labels[i] && labels[i][0] ? CrossViMetrics::values.buttonHintsHeight : smallButtonHeight;
    renderer.fillRoundedRect(positions[i], y, buttonWidth, buttonHeight, CORNER_RADIUS, Color::White);
    renderer.drawRoundedRect(positions[i], y, buttonWidth, buttonHeight, 1, CORNER_RADIUS, true, true, false, false,
                             true);
    if (labels[i] && labels[i][0]) {
      const int width = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      renderer.drawText(SMALL_FONT_ID, positions[i] + (buttonWidth - 1 - width) / 2, y + textYOffset, labels[i]);
    }
  }
  renderer.setOrientation(orientation);
}

void CrossViTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  constexpr int buttonWidth = CrossViMetrics::values.sideButtonHintsWidth;
  constexpr int buttonHeight = 78;
  if (gpio.deviceIsX3()) {
    constexpr int y = 155;
    const char* labels[] = {topBtn, bottomBtn};
    const int xs[] = {0, renderer.getScreenWidth() - buttonWidth};
    for (int i = 0; i < 2; ++i) {
      if (!labels[i] || !labels[i][0]) continue;
      renderer.drawRoundedRect(xs[i], y, buttonWidth, buttonHeight, 1, CORNER_RADIUS, i == 1, i == 0, i == 1, i == 0,
                               true);
      const int width = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, xs[i], y + (buttonHeight + width) / 2, labels[i]);
    }
    return;
  }
  const int x = renderer.getScreenWidth() - buttonWidth;
  const char* labels[] = {topBtn, bottomBtn};
  for (int i = 0; i < 2; ++i) {
    if (!labels[i] || !labels[i][0]) continue;
    const int y = TOP_HINT_BUTTON_Y + i * (buttonHeight + 5);
    renderer.drawRoundedRect(x, y, buttonWidth, buttonHeight, 1, CORNER_RADIUS, true, false, true, false, true);
    const int width = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
    renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + width) / 2, labels[i]);
  }
}

void CrossViTheme::drawHomeHeader(const GfxRenderer& renderer, const Rect rect, const char* title) const {
  (void)title;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t batteryPercentage = powerManager.getBatteryPercentage();
  const Rect battery = topRightBatteryRect(rect);
  const int batteryLeft = drawBatteryRight(renderer, battery, batteryPercentage, showBatteryPercentage);

  char timeValue[40]{};
  const bool showDateTime = outsideDateTimeText(timeValue);
  const int dateTimeWidth = showDateTime ? renderer.getTextWidth(SMALL_FONT_ID, timeValue) : 0;
  const int rightLimit = showDateTime ? batteryLeft - dateTimeWidth - 18 : batteryLeft - 10;

  if (SETTINGS.showDeviceNameOnHome) {
    const char* displayName = SETTINGS.deviceDisplayName[0] != '\0' ? SETTINGS.deviceDisplayName
                                                                    : (gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4");
    const int left = rect.x + CrossViMetrics::values.contentSidePadding;
    const int nameMaxWidth = std::max(0, rightLimit - left);
    const std::string safeName =
        renderer.truncatedText(UI_10_FONT_ID, displayName, nameMaxWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, left, rect.y + 9, safeName.c_str());
  }
  if (showDateTime) {
    drawOutsideClockRight(renderer, rect, batteryLeft, timeValue);
  }
}

Rect CrossViTheme::getHomeCoverCacheRect(const Rect tileRect) const { return CrossViLayout::calculate(tileRect).cover; }

void CrossViTheme::drawHomeContent(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                   const HomeBookSummary& summary) const {
  const CrossViLayout layout = CrossViLayout::calculate(rect);
  const bool hasBook = !recentBooks.empty();
  const bool selected = hasBook && selectorIndex == 0;

  renderer.drawRoundedRect(layout.card.x, layout.card.y, layout.card.width, layout.card.height, selected ? 2 : 1,
                           CORNER_RADIUS, true, true, true, true, true);

  if (!hasBook) {
    const int iconX = layout.card.x + 22;
    const int iconY = layout.card.y + (layout.card.height - 32) / 2;
    renderer.drawIcon(CoverIcon, iconX, iconY, 32);
    const int textX = iconX + 48;
    const int textWidth = std::max(0, layout.card.x + layout.card.width - textX - 20);
    const std::string titleText =
        renderer.truncatedText(UI_12_FONT_ID, tr(STR_NO_OPEN_BOOK), textWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, layout.card.y + layout.card.height / 2 - 28, titleText.c_str(), true,
                      EpdFontFamily::BOLD);
    const std::string help = renderer.truncatedText(UI_10_FONT_ID, tr(STR_START_READING), textWidth);
    renderer.drawText(UI_10_FONT_ID, textX, layout.card.y + layout.card.height / 2 + 8, help.c_str());
    return;
  }

  const RecentBook& book = recentBooks.front();
  if (!bufferRestored || !coverRendered) {
    bool drewCover = false;
    if (!book.coverBmpPath.empty()) {
      HalFile file;
      if (openExistingCover(book.coverBmpPath, 240, "CROSSVI", file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height);
          drewCover = true;
        }
      }
    }
    if (!drewCover) {
      renderer.fillRoundedRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height, 3,
                               Color::LightGray);
      const int coverLabelHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int placeholderHeight = 32 + 8 + coverLabelHeight;
      const int placeholderY = layout.cover.y + std::max(0, (layout.cover.height - placeholderHeight) / 2);
      renderer.drawIcon(CoverIcon, layout.cover.x + std::max(0, (layout.cover.width - 32) / 2), placeholderY, 32);
      const Rect coverLabel{layout.cover.x, placeholderY + 40, layout.cover.width, coverLabelHeight};
      drawCenteredText(renderer, SMALL_FONT_ID, coverLabel, tr(STR_COVER));
    }
    if (layout.cover.width > 0 && layout.cover.height > 0) {
      renderer.drawRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height, true);
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  const std::string displayTitle = displayTitleForBook(book);
  int titleFontId = UI_12_FONT_ID;
  std::vector<std::string> titleLines =
      renderer.wrappedText(titleFontId, displayTitle.c_str(), layout.title.width, 2, EpdFontFamily::BOLD);
  if (finalLineIsEllipsized(titleLines)) {
    titleFontId = UI_10_FONT_ID;
    titleLines = renderer.wrappedText(titleFontId, displayTitle.c_str(), layout.title.width, 2, EpdFontFamily::BOLD);
  }
  int titleY = layout.title.y;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  for (const std::string& line : titleLines) {
    renderer.drawText(titleFontId, layout.title.x, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineHeight;
  }

  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const HomeBookAction action = homeBookAction(summary);
  const bool hasTrustedReadingTime = summary.bookStatsState == DashboardMetricState::Available ||
                                     summary.bookStatsState == DashboardMetricState::NoData;
  if (action == HomeBookAction::Start || (summary.hasStartedReading && hasTrustedReadingTime)) {
    const std::string primary =
        action == HomeBookAction::Start
            ? std::string(tr(STR_HOME_NOT_STARTED))
            : (summary.bookReadingSeconds > 0 ? formatHomeReadingTime(summary.bookReadingSeconds)
                                              : std::string(tr(STR_HOME_IN_PROGRESS)));
    const std::string safePrimary = renderer.truncatedText(SMALL_FONT_ID, primary.c_str(), layout.stats.width);
    renderer.drawText(SMALL_FONT_ID, layout.stats.x,
                      layout.stats.y + std::max(0, (layout.stats.height - smallLineHeight) / 2), safePrimary.c_str());
  }

  const bool showProgress = hasReliableHomeBookProgress(summary) && summary.hasStartedReading;
  if (showProgress && layout.progressBar.width > 0 && layout.progressBar.height >= 10) {
    constexpr int BAR_HEIGHT = 10;
    const int barY = layout.progressBar.y + std::max(0, (layout.progressBar.height - BAR_HEIGHT) / 2);
    renderer.drawRoundedRect(layout.progressBar.x, barY, layout.progressBar.width, BAR_HEIGHT, 1, 3, true, true, true,
                             true, true);
    const int fillWidth = DashboardProgress::fillWidth(layout.progressBar.width, summary.progressPercent);
    if (fillWidth > 0) {
      renderer.fillRoundedRect(layout.progressBar.x + 2, barY + 2, fillWidth, BAR_HEIGHT - 4, 1, Color::Black);
    }
    char progressValue[12]{};
    if (summary.progressBelowOnePercent) {
      snprintf(progressValue, sizeof(progressValue), "<1%%");
    } else {
      snprintf(progressValue, sizeof(progressValue), "%u%%", static_cast<unsigned>(summary.progressPercent));
    }
    const int valueWidth = renderer.getTextWidth(SMALL_FONT_ID, progressValue);
    renderer.drawText(SMALL_FONT_ID, layout.progressLabel.x + std::max(0, layout.progressLabel.width - valueWidth),
                      layout.progressLabel.y + std::max(0, (layout.progressLabel.height - smallLineHeight) / 2),
                      progressValue);
  }

  Rect continueButton = showProgress ? layout.continueButton : layout.continueButtonWithoutProgress;
  if (continueButton.height > 0) {
    const char* actionText = I18N.get(homeBookActionLabelId(summary));
    const int actionWidth = renderer.getTextWidth(UI_10_FONT_ID, actionText, EpdFontFamily::BOLD);
    const int maxButtonWidth = std::max(0, layout.details.width - 16);
    continueButton.width = std::min(maxButtonWidth, std::max(208, actionWidth + 24));
    continueButton.x = layout.details.x + std::max(0, (layout.details.width - continueButton.width) / 2);
    if (selected) {
      renderer.fillRoundedRect(continueButton.x, continueButton.y, continueButton.width, continueButton.height,
                               CORNER_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(continueButton.x, continueButton.y, continueButton.width, continueButton.height, 1,
                               CORNER_RADIUS, true, true, true, true, true);
    }
    int renderedActionWidth = 0;
    const std::string action = renderer.truncatedText(UI_10_FONT_ID, actionText, std::max(0, continueButton.width - 24),
                                                      EpdFontFamily::BOLD, &renderedActionWidth);
    drawCenteredText(renderer, UI_10_FONT_ID, continueButton, action.c_str(), !selected, EpdFontFamily::BOLD,
                     renderedActionWidth);
  }
}

void CrossViTheme::drawHomeRecentList(GfxRenderer& renderer, const Rect rect,
                                      const std::vector<RecentBook>& recentBooks, const int selectedBookIndex) const {
  const CrossViRecentListLayout layout = CrossViRecentListLayout::calculate(rect, static_cast<int>(recentBooks.size()));
  renderer.drawRoundedRect(layout.card.x, layout.card.y, layout.card.width, layout.card.height, 1, CORNER_RADIUS, true,
                           true, true, true, true);

  constexpr int HEADER_ICON_SIZE = 28;
  const int headerIconY = layout.header.y + std::max(0, (layout.header.height - HEADER_ICON_SIZE) / 2);
  drawRecentHistoryIcon(renderer, layout.header.x + 4, headerIconY);
  const int headerTextX = layout.header.x + HEADER_ICON_SIZE + 14;
  const std::string heading = renderer.truncatedText(
      UI_10_FONT_ID, tr(STR_HOME_RECENTLY_READ), std::max(0, layout.header.x + layout.header.width - headerTextX - 4));
  renderer.drawText(UI_10_FONT_ID, headerTextX,
                    layout.header.y + std::max(0, (layout.header.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2),
                    heading.c_str());

  renderer.drawLine(layout.list.x, layout.list.y, layout.list.x + layout.list.width - 1, layout.list.y, true);

  if (layout.visibleRows == 0) {
    const std::string empty = renderer.truncatedText(UI_10_FONT_ID, tr(STR_NO_RECENT_BOOKS), layout.list.width - 24);
    drawCenteredText(renderer, UI_10_FONT_ID, layout.list, empty.c_str());
    return;
  }

  constexpr int ROW_ICON_SIZE = 24;
  constexpr int ROW_SIDE_PADDING = 12;
  constexpr int TEXT_ICON_GAP = 12;
  constexpr int CHEVRON_WIDTH = 8;
  constexpr int CHEVRON_HEIGHT = 12;
  constexpr int CHEVRON_RIGHT = 14;
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  for (int index = 0; index < layout.visibleRows; ++index) {
    const Rect row = layout.row(index);
    if (index > 0) renderer.drawLine(row.x, row.y, row.x + row.width - 1, row.y, true);
    const bool selected = index == selectedBookIndex;
    if (selected) {
      renderer.fillRoundedRect(row.x + 2, row.y + 2, std::max(0, row.width - 4), std::max(0, row.height - 4), 4,
                               Color::Black);
    }

    const RecentBook& book = recentBooks[index];
    const int iconX = row.x + ROW_SIDE_PADDING;
    const int iconY = row.y + std::max(0, (row.height - ROW_ICON_SIZE) / 2);
    const uint8_t* icon = iconForName(UITheme::getFileIcon(book.path), ROW_ICON_SIZE);
    drawMenuIcon(renderer, icon ? icon : File24Icon, iconX, iconY, ROW_ICON_SIZE, !selected);

    const int chevronX = row.x + row.width - CHEVRON_RIGHT - CHEVRON_WIDTH;
    const int chevronY = row.y + (row.height - CHEVRON_HEIGHT) / 2;
    renderer.drawLine(chevronX, chevronY, chevronX + CHEVRON_WIDTH - 1, chevronY + CHEVRON_HEIGHT / 2, !selected);
    renderer.drawLine(chevronX + CHEVRON_WIDTH - 1, chevronY + CHEVRON_HEIGHT / 2, chevronX, chevronY + CHEVRON_HEIGHT,
                      !selected);

    const int textX = iconX + ROW_ICON_SIZE + TEXT_ICON_GAP;
    const int textWidth = std::max(0, chevronX - 10 - textX);
    const std::string title = renderer.truncatedText(UI_10_FONT_ID, displayTitleForBook(book).c_str(), textWidth);
    const char* rawSubtitle = book.author.empty() ? recentBookFormatLabel(book) : book.author.c_str();
    const std::string subtitle = renderer.truncatedText(SMALL_FONT_ID, rawSubtitle, textWidth);
    const int textHeight = titleLineHeight + subtitleLineHeight;
    const int textY = row.y + std::max(0, (row.height - textHeight) / 2);
    renderer.drawText(UI_10_FONT_ID, textX, textY, title.c_str(), !selected);
    renderer.drawText(SMALL_FONT_ID, textX, textY + titleLineHeight, subtitle.c_str(), !selected);
  }
}

void CrossViTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount,
                                  const int selectedIndex, const std::function<const char*(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int tileWidth = (rect.width - MENU_SIDE_PADDING * 2 - MENU_GAP) / MENU_COLUMNS;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const HomeMenuLayout::Fit layout =
      HomeMenuLayout::fit(rect.height, buttonCount, MENU_COLUMNS, CrossViMetrics::values.menuRowHeight,
                          CrossViMetrics::values.menuSpacing, std::max(MENU_ICON_SIZE, lineHeight) + 12);
  const int menuOffset = std::min(24, std::max(0, (rect.height - layout.totalHeight()) / 2));

  for (int i = 0; i < buttonCount; ++i) {
    const int column = i % MENU_COLUMNS;
    const Rect tile{rect.x + MENU_SIDE_PADDING + column * (tileWidth + MENU_GAP),
                    rect.y + menuOffset + layout.yOffset(i, MENU_COLUMNS), tileWidth, layout.rowHeight};
    const bool selected = i == selectedIndex;

    if (selected) {
      renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, CORNER_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, CORNER_RADIUS, true, true, true, true, true);
    }

    int contentX = tile.x + MENU_CONTENT_PADDING;
    if (rowIcon) {
      const UIIcon iconName = rowIcon(i);
      if (iconName == UIIcon::Book) {
        drawStatsIcon(renderer, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, !selected);
        contentX += MENU_ICON_SIZE + 10;
      } else if (iconName == UIIcon::Bookmark) {
        drawBookmarkIcon(renderer, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, !selected);
        contentX += MENU_ICON_SIZE + 10;
      } else if (const uint8_t* icon = iconForName(iconName)) {
        drawMenuIcon(renderer, icon, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, MENU_ICON_SIZE, !selected);
        contentX += MENU_ICON_SIZE + 10;
      }
    }

    const char* rawLabel = buttonLabel(i);
    const EpdFontFamily::Style style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label =
        renderer.truncatedText(UI_10_FONT_ID, rawLabel, std::max(0, tile.x + tile.width - contentX - 10), style);
    renderer.drawText(UI_10_FONT_ID, contentX, tile.y + (tile.height - lineHeight) / 2, label.c_str(), !selected,
                      style);
  }
}

void CrossViTheme::drawHomeTripleCovers(GfxRenderer& renderer, const Rect rect,
                                        const std::vector<RecentBook>& recentBooks, int currentBookIndex,
                                        const bool bookSelected) const {
  const CrossViTripleCoverLayout layout = CrossViTripleCoverLayout::calculate(rect);

  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount <= 0) {
    renderer.drawIcon(CoverIcon, layout.card.x + (layout.card.width - MENU_ICON_SIZE) / 2,
                      layout.card.y + layout.card.height / 2 - MENU_ICON_SIZE - 4, MENU_ICON_SIZE);
    const std::string empty =
        renderer.truncatedText(UI_10_FONT_ID, tr(STR_NO_OPEN_BOOK), std::max(0, layout.card.width - 40));
    drawCenteredText(renderer, UI_10_FONT_ID,
                     Rect{layout.card.x + 20, layout.card.y + layout.card.height / 2 + 4,
                          std::max(0, layout.card.width - 40), renderer.getLineHeight(UI_10_FONT_ID)},
                     empty.c_str());
    return;
  }

  currentBookIndex = std::clamp(currentBookIndex, 0, bookCount - 1);
  const auto bookIndices = CrossViTripleCoverLayout::fixedBookIndices(bookCount);
  const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  for (int slot = 0; slot < CrossViTripleCoverLayout::ITEM_COUNT; ++slot) {
    const int bookIndex = bookIndices[slot];
    if (bookIndex < 0 || bookIndex >= bookCount) continue;
    const bool selected = bookSelected && bookIndex == currentBookIndex;
    if (selected) {
      const Rect item = layout.items[slot];
      renderer.fillRoundedRect(item.x + 2, item.y + 2, std::max(0, item.width - 4), std::max(0, item.height - 4),
                               CORNER_RADIUS, Color::LightGray);
    }
    drawCarouselBookCover(renderer, recentBooks[bookIndex], layout.covers[slot], selected, false, 0, 240);

    const Rect titleRect = layout.titles[slot];
    const std::string title = displayTitleForBook(recentBooks[bookIndex]);
    const std::vector<std::string> lines = renderer.wrappedText(SMALL_FONT_ID, title.c_str(), titleRect.width, 2);
    int titleY = titleRect.y + std::max(0, (titleRect.height - static_cast<int>(lines.size()) * titleLineHeight) / 2);
    for (const std::string& line : lines) {
      const int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      renderer.drawText(SMALL_FONT_ID, titleRect.x + std::max(0, (titleRect.width - titleWidth) / 2), titleY,
                        line.c_str());
      titleY += titleLineHeight;
    }
  }
}

void CrossViTheme::drawHomeCarousel(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                    int currentBookIndex, const bool bookSelected, const int buttonCount,
                                    const int selectedButtonIndex, const std::function<UIIcon(int index)>& buttonIcon,
                                    const bool drawBookArea) const {
  const CrossViCarouselLayout layout = CrossViCarouselLayout::calculate(rect);
  const int bookCount = static_cast<int>(recentBooks.size());
  if (drawBookArea && bookCount > 0) {
    currentBookIndex = std::clamp(currentBookIndex, 0, bookCount - 1);
    const auto sideBooks = CrossViCarouselLayout::carouselSideBookIndices(currentBookIndex, bookCount);
    if (sideBooks[0] >= 0) {
      drawCarouselBookCover(renderer, recentBooks[sideBooks[0]], layout.previousCover, false, true, 1, 240);
    }
    if (sideBooks[1] >= 0) {
      drawCarouselBookCover(renderer, recentBooks[sideBooks[1]], layout.nextCover, false, true, -1, 240);
    }
    const RecentBook& currentBook = recentBooks[currentBookIndex];
    const char* missingCoverText =
        FsHelpers::hasEpubExtension(currentBook.path) ? tr(STR_COVER_LOADS_WHEN_OPENED) : nullptr;
    drawCarouselBookCover(renderer, currentBook, layout.currentCover, bookSelected, false, 0,
                          layout.currentCover.height, missingCoverText);

    constexpr int DOT_GAP = 10;
    const int dotCount = std::min(bookCount, 3);
    const int selectedDot = CrossViCarouselLayout::selectedDotIndex(currentBookIndex, bookCount);
    const int dotsWidth = dotCount * CAROUSEL_DOT_SIZE + (dotCount - 1) * DOT_GAP;
    int dotX = layout.pageDots.x + (layout.pageDots.width - dotsWidth) / 2;
    const int dotY = layout.pageDots.y + (layout.pageDots.height - CAROUSEL_DOT_SIZE) / 2;
    for (int index = 0; index < dotCount; ++index) {
      drawCarouselDot(renderer, dotX, dotY, bookSelected && index == selectedDot);
      dotX += CAROUSEL_DOT_SIZE + DOT_GAP;
    }

    int authorWidth = 0;
    const std::string author = currentBook.author.empty()
                                   ? std::string{}
                                   : renderer.truncatedText(UI_10_FONT_ID, currentBook.author.c_str(),
                                                            layout.author.width, EpdFontFamily::REGULAR, &authorWidth);
    const std::string title = displayTitleForBook(currentBook);
    int safeTitleWidth = 0;
    const std::string safeTitle =
        renderer.truncatedText(UI_12_FONT_ID, title.c_str(), layout.title.width, EpdFontFamily::BOLD, &safeTitleWidth);
    if (bookSelected) {
      constexpr int FOCUS_HORIZONTAL_PADDING = 12;
      constexpr int FOCUS_VERTICAL_PADDING = 4;
      const int bandWidth = std::min(layout.title.width, safeTitleWidth + FOCUS_HORIZONTAL_PADDING * 2);
      const int bandHeight =
          std::min(layout.title.height, renderer.getLineHeight(UI_12_FONT_ID) + FOCUS_VERTICAL_PADDING * 2);
      renderer.fillRoundedRect(layout.title.x + (layout.title.width - bandWidth) / 2,
                               layout.title.y + (layout.title.height - bandHeight) / 2, bandWidth, bandHeight,
                               CORNER_RADIUS, Color::Black);
    }
    if (!author.empty())
      drawCenteredText(renderer, UI_10_FONT_ID, layout.author, author.c_str(), true, EpdFontFamily::REGULAR,
                       authorWidth);
    drawCenteredText(renderer, UI_12_FONT_ID, layout.title, safeTitle.c_str(), !bookSelected, EpdFontFamily::BOLD,
                     safeTitleWidth);
  } else if (drawBookArea) {
    renderer.fillRoundedRect(layout.currentCover.x, layout.currentCover.y, layout.currentCover.width,
                             layout.currentCover.height, CORNER_RADIUS, Color::LightGray);
    renderer.drawIcon(CoverIcon, layout.currentCover.x + (layout.currentCover.width - MENU_ICON_SIZE) / 2,
                      layout.currentCover.y + (layout.currentCover.height - MENU_ICON_SIZE) / 2, MENU_ICON_SIZE);
    const std::string empty =
        renderer.truncatedText(UI_12_FONT_ID, tr(STR_NO_OPEN_BOOK), layout.title.width, EpdFontFamily::BOLD);
    drawCenteredText(renderer, UI_12_FONT_ID, layout.title, empty.c_str(), true, EpdFontFamily::BOLD);
  }

  if (buttonCount <= 0 || layout.menu.width <= 0 || layout.menu.height <= 0) return;
  constexpr int BUTTON_GAP = 6;
  const int buttonWidth = std::max(1, (layout.menu.width - BUTTON_GAP * (buttonCount - 1)) / buttonCount);
  const int iconSize = MENU_ICON_SIZE;
  for (int index = 0; index < buttonCount; ++index) {
    const Rect button{layout.menu.x + index * (buttonWidth + BUTTON_GAP), layout.menu.y, buttonWidth,
                      layout.menu.height};
    const bool selected = index == selectedButtonIndex;
    if (selected) {
      renderer.fillRoundedRect(button.x, button.y, button.width, button.height, CORNER_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 1, CORNER_RADIUS, true, true, true,
                               true, true);
    }

    const UIIcon iconName = buttonIcon ? buttonIcon(index) : UIIcon::None;
    const int iconX = button.x + (button.width - iconSize) / 2;
    const int iconY = button.y + (button.height - iconSize) / 2;
    if (iconName == UIIcon::Book) {
      drawStatsIcon(renderer, iconX, iconY, !selected);
    } else if (iconName == UIIcon::Bookmark) {
      drawBookmarkIcon(renderer, iconX, iconY, !selected);
    } else if (const uint8_t* icon = iconForName(iconName)) {
      drawMenuIcon(renderer, icon, iconX, iconY, iconSize, !selected);
    }
  }
}
