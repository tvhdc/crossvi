#include "LibraryGridView.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "components/LibraryGridModel.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/pin.h"
#include "components/icons/text24.h"
#include "fontIds.h"

namespace {
constexpr int COVER_FOCUS_INSET = 4;

struct GridLayoutSpec {
  int horizontalPadding;
  int columnGap;
  int rowGap;
  int targetCoverWidth;
  int targetCoverHeight;
  int perCoverTitleHeight;
  int titleGap;
  int selectedBannerHeight;
  int selectedBannerGap;
  int topInset;
};

struct GridLayout {
  LibraryGridShape shape;
  GridLayoutSpec spec;
  int startX;
  int gridStartY;
  int cellWidth;
  int rowHeight;
  int coverWidth;
  int coverHeight;
};

constexpr GridLayoutSpec layoutSpec(const uint8_t gridSetting, const int screenWidth) {
  (void)gridSetting;
  const bool x3 = screenWidth >= 500;
  // 3x2: large covers with an independent two-line title block below each.
  return x3 ? GridLayoutSpec{18, 22, 17, 149, 240, 46, 4, 0, 0, 0}
            : GridLayoutSpec{12, 12, 24, 144, 240, 46, 4, 0, 0, 0};
}

GridLayout calculateLayout(const Rect rect, const uint8_t gridSetting) {
  const LibraryGridShape shape = LibraryGridModel::shape(gridSetting);
  const GridLayoutSpec spec = layoutSpec(gridSetting, rect.width);
  const int cellWidth =
      std::max(1, (rect.width - spec.horizontalPadding * 2 - spec.columnGap * (shape.columns - 1)) / shape.columns);
  const int usedWidth = cellWidth * shape.columns + spec.columnGap * (shape.columns - 1);
  const int bannerBlock = spec.selectedBannerHeight > 0 ? spec.selectedBannerHeight + spec.selectedBannerGap : 0;
  const int reservedPerRow = spec.perCoverTitleHeight > 0 ? spec.perCoverTitleHeight + spec.titleGap : 0;
  constexpr int focusReserve = COVER_FOCUS_INSET * 2;
  const int availableCoverHeight = std::max(
      1, (rect.height - spec.topInset - bannerBlock - focusReserve - spec.rowGap * (shape.rows - 1)) / shape.rows -
             reservedPerRow);
  // Focus is drawn into the intentional inter-cell gap, so it must not make
  // the physical cover smaller than the profile's target size.
  const int maxCoverWidth = std::max(1, cellWidth);
  const int maxCoverHeight = std::max(1, availableCoverHeight);

  int coverWidth = std::min(spec.targetCoverWidth, maxCoverWidth);
  int coverHeight = coverWidth * spec.targetCoverHeight / spec.targetCoverWidth;
  if (coverHeight > maxCoverHeight) {
    coverHeight = maxCoverHeight;
    coverWidth = coverHeight * spec.targetCoverWidth / spec.targetCoverHeight;
  }
  coverWidth = std::max(1, coverWidth);
  coverHeight = std::max(1, coverHeight);

  const int rowHeight = coverHeight + reservedPerRow;
  const int usedHeight = rowHeight * shape.rows + spec.rowGap * (shape.rows - 1);
  const int gridStartY = spec.selectedBannerHeight > 0
                             ? rect.y + spec.topInset + bannerBlock + COVER_FOCUS_INSET
                             : rect.y + COVER_FOCUS_INSET + std::max(0, (rect.height - focusReserve - usedHeight) / 2);
  return {shape,      spec,       rect.x + (rect.width - usedWidth) / 2, gridStartY, cellWidth, rowHeight,
          coverWidth, coverHeight};
}

bool drawCoverBitmap(const GfxRenderer& renderer, const LibraryBookRecord& book, const Rect cover) {
  if (book.coverBmpPath.empty()) return false;
  const uint32_t started = static_cast<uint32_t>(millis());
  const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, cover.height);
  HalFile file;
  if (!Storage.openFileForRead("LIBGRID", path, file)) {
    // Reuse the canonical 240 px EPUB thumbnail before trying the legacy Home
    // cache. No cover generation is performed while the render lock is held.
    const std::string canonicalPath = UITheme::getCoverThumbPath(book.coverBmpPath, 240);
    constexpr int HOME_THUMB_HEIGHT = 168;
    const std::string fallbackPath = UITheme::getCoverThumbPath(book.coverBmpPath, HOME_THUMB_HEIGHT);
    const bool canonicalOpened = canonicalPath != path && Storage.openFileForRead("LIBGRID", canonicalPath, file);
    const bool fallbackOpened = !canonicalOpened && fallbackPath != path && fallbackPath != canonicalPath &&
                                Storage.openFileForRead("LIBGRID", fallbackPath, file);
    if (!canonicalOpened && !fallbackOpened) {
      LOG_DBG("COVR", "GRID cache_miss requested=%s canonical=%s fallback=%s elapsed_ms=%u", path.c_str(),
              canonicalPath.c_str(), fallbackPath.c_str(),
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
      return false;
    }
    LOG_DBG("COVR", "GRID cache_fallback requested=%s using=%s open_ms=%u", path.c_str(),
            canonicalOpened ? canonicalPath.c_str() : fallbackPath.c_str(),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
  }
  Bitmap bitmap(file);
  const uint32_t parseStart = static_cast<uint32_t>(millis());
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_DBG("COVR", "GRID parse_failed path=%s parse_ms=%u", path.c_str(),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - parseStart));
    return false;
  }
  const uint32_t parseMs = static_cast<uint32_t>(millis()) - parseStart;
  const int sourceWidth = bitmap.getWidth();
  const int sourceHeight = bitmap.getHeight();
  if (sourceWidth <= 0 || sourceHeight <= 0) return false;
  const float sourceRatio = static_cast<float>(sourceWidth) / sourceHeight;
  const float coverRatio = static_cast<float>(cover.width) / cover.height;
  // Generated library thumbnails use the same portrait profile as the grid,
  // but integer rounding makes their ratio a few percent narrower. Avoid a
  // crop in that case so the renderer can use its one-read 1-bit fast path.
  // A larger mismatch still uses center-crop to preserve the existing cover
  // framing contract.
  if (bitmap.is1Bit() && std::fabs(sourceRatio - coverRatio) <= 0.05f) {
    const float fitScale = std::min(
        {1.0f, static_cast<float>(cover.width) / sourceWidth, static_cast<float>(cover.height) / sourceHeight});
    const int drawnWidth = std::max(1, static_cast<int>(std::floor(sourceWidth * fitScale)));
    const int drawnHeight = std::max(1, static_cast<int>(std::floor(sourceHeight * fitScale)));
    renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
    renderer.drawBitmap(bitmap, cover.x + (cover.width - drawnWidth) / 2, cover.y + (cover.height - drawnHeight) / 2,
                        cover.width, cover.height);
    LOG_DBG("COVR", "GRID draw path=%s parse_ms=%u draw_ms=%u total_ms=%u mode=fit", path.c_str(),
            static_cast<unsigned>(parseMs),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - parseStart - parseMs),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
    return true;
  }
  float cropX = sourceRatio > coverRatio ? 1.0f - coverRatio / sourceRatio : 0.0f;
  float cropY = sourceRatio < coverRatio ? 1.0f - sourceRatio / coverRatio : 0.0f;
  const float croppedWidth = sourceWidth * (1.0f - cropX);
  const float croppedHeight = sourceHeight * (1.0f - cropY);
  const float scale = std::min(
      {1.0f, static_cast<float>(cover.width) / croppedWidth, static_cast<float>(cover.height) / croppedHeight});
  const int drawnWidth = std::max(1, static_cast<int>(std::floor(croppedWidth * scale)));
  const int drawnHeight = std::max(1, static_cast<int>(std::floor(croppedHeight * scale)));
  renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
  renderer.drawBitmap(bitmap, cover.x + (cover.width - drawnWidth) / 2, cover.y + (cover.height - drawnHeight) / 2,
                      cover.width, cover.height, cropX, cropY);
  LOG_DBG("COVR", "GRID draw path=%s parse_ms=%u draw_ms=%u total_ms=%u mode=crop", path.c_str(),
          static_cast<unsigned>(parseMs), static_cast<unsigned>(static_cast<uint32_t>(millis()) - parseStart - parseMs),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
  return true;
}

const char* formatLabel(const LibraryBookFormat format) {
  switch (format) {
    case LibraryBookFormat::Text:
      return "TXT";
    case LibraryBookFormat::Markdown:
      return "MD";
    case LibraryBookFormat::Xtc:
      return "XTC";
    case LibraryBookFormat::Xtch:
      return "XTCH";
    case LibraryBookFormat::Epub:
    default:
      return "EPUB";
  }
}

void drawPlaceholder(const GfxRenderer& renderer, const LibraryBookRecord& book, const Rect cover) {
  renderer.fillRoundedRect(cover.x, cover.y, cover.width, cover.height, 3, Color::LightGray);
  const bool text = book.format == LibraryBookFormat::Text || book.format == LibraryBookFormat::Markdown;
  const uint8_t* icon = text ? Text24Icon : BookIcon;
  const int size = text ? 24 : 32;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int groupHeight = size + 4 + lineHeight;
  const int iconY = cover.y + std::max(0, (cover.height - groupHeight) / 2);
  renderer.drawIcon(icon, cover.x + (cover.width - size) / 2, iconY, size);
  const char* label = formatLabel(book.format);
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  renderer.drawText(SMALL_FONT_ID, cover.x + std::max(0, (cover.width - labelWidth) / 2), iconY + size + 4, label);
}

void drawPinnedBadge(const GfxRenderer& renderer, const Rect cover) {
  constexpr int width = 16;
  constexpr int height = 16;
  constexpr int x = 3;
  const int badgeX = cover.x + cover.width - width - x;
  const int badgeY = cover.y + x;
  renderer.fillRect(badgeX - 2, badgeY - 2, width + 4, height + 4, false);
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const uint8_t byte = PinStatusIcon[row * 2 + (col >> 3)];
      renderer.drawPixel(badgeX + col, badgeY + row, (byte & (1U << (7 - (col % 8)))) == 0);
    }
  }
}

bool drawCoverCell(const GfxRenderer& renderer, const GridLayout& layout, const LibraryBookRecord& book,
                   const size_t index, const uint8_t gridSetting, const bool renderCover) {
  const int column = static_cast<int>(index) % layout.shape.columns;
  const int row = static_cast<int>(index) / layout.shape.columns;
  const Rect cell{layout.startX + column * (layout.cellWidth + layout.spec.columnGap),
                  layout.gridStartY + row * (layout.rowHeight + layout.spec.rowGap), layout.cellWidth,
                  layout.rowHeight};
  const Rect cover{cell.x + (cell.width - layout.coverWidth) / 2, cell.y, layout.coverWidth, layout.coverHeight};
  const bool coverDrawn = renderCover && drawCoverBitmap(renderer, book, cover);
  if (!coverDrawn) drawPlaceholder(renderer, book, cover);
  renderer.maskRoundedRectOutsideCorners(cover.x, cover.y, cover.width, cover.height, 3);
  renderer.drawRoundedRect(cover.x, cover.y, cover.width, cover.height, 1, 3, true, true, true, true, true);
  if (book.pinned) drawPinnedBadge(renderer, cover);

  if (LibraryGridModel::usesPerCoverTitles(gridSetting)) {
    const auto lines = renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), cell.width, 2);
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    int y = cover.y + cover.height + layout.spec.titleGap +
            std::max(0, (layout.spec.perCoverTitleHeight - lineHeight * static_cast<int>(lines.size())) / 2);
    for (const auto& line : lines) {
      const int width = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      renderer.drawText(SMALL_FONT_ID, cell.x + (cell.width - width) / 2, y, line.c_str());
      y += lineHeight;
    }
  }
  return coverDrawn;
}
}  // namespace

size_t LibraryGridView::pageSize(const uint8_t gridSetting) { return LibraryGridModel::pageSize(gridSetting); }

int LibraryGridView::coverHeight(const Rect rect, const uint8_t gridSetting) {
  return calculateLayout(rect, gridSetting).coverHeight;
}

uint8_t LibraryGridView::drawStatic(const GfxRenderer& renderer, const Rect rect,
                                    const std::vector<LibraryBookRecord>& books, const uint8_t gridSetting,
                                    const bool renderCovers) {
  const GridLayout layout = calculateLayout(rect, gridSetting);

  uint8_t renderedMask = 0;
  for (size_t i = 0; i < books.size(); ++i) {
    const bool rendered = drawCoverCell(renderer, layout, books[i], i, gridSetting, renderCovers);
    if (i < 8 && rendered) {
      renderedMask |= static_cast<uint8_t>(1U << i);
    }
  }
  return renderedMask;
}

void LibraryGridView::drawCoverAt(const GfxRenderer& renderer, const Rect rect,
                                  const std::vector<LibraryBookRecord>& books, const size_t index,
                                  const uint8_t gridSetting) {
  if (index >= books.size()) return;
  const GridLayout layout = calculateLayout(rect, gridSetting);
  drawCoverCell(renderer, layout, books[index], index, gridSetting, true);
}

void LibraryGridView::drawSelection(const GfxRenderer& renderer, const Rect rect,
                                    const std::vector<LibraryBookRecord>& books, const size_t selectedOnPage,
                                    const uint8_t gridSetting) {
  const GridLayout layout = calculateLayout(rect, gridSetting);
  if (selectedOnPage >= books.size()) return;
  const int column = static_cast<int>(selectedOnPage) % layout.shape.columns;
  const int row = static_cast<int>(selectedOnPage) / layout.shape.columns;
  const Rect cell{layout.startX + column * (layout.cellWidth + layout.spec.columnGap),
                  layout.gridStartY + row * (layout.rowHeight + layout.spec.rowGap), layout.cellWidth,
                  layout.rowHeight};
  const Rect cover{cell.x + (cell.width - layout.coverWidth) / 2, cell.y, layout.coverWidth, layout.coverHeight};
  const Rect focus{cover.x - COVER_FOCUS_INSET, cover.y - COVER_FOCUS_INSET, cover.width + COVER_FOCUS_INSET * 2,
                   cover.height + COVER_FOCUS_INSET * 2};
  renderer.drawRoundedRect(focus.x, focus.y, focus.width, focus.height, 2, 5, true, true, true, true, true);
  renderer.fillRect(focus.x, focus.y + 5, 4, std::max(0, focus.height - 10), true);
}

void LibraryGridView::draw(const GfxRenderer& renderer, const Rect rect, const std::vector<LibraryBookRecord>& books,
                           const size_t selectedOnPage, const uint8_t gridSetting) {
  drawStatic(renderer, rect, books, gridSetting);
  drawSelection(renderer, rect, books, selectedOnPage, gridSetting);
}
