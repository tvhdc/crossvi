#pragma once

#include <algorithm>
#include <array>

#include "components/themes/BaseTheme.h"

// Pure geometry for the CrossVi Home bookplate. Keeping the measurements out
// of the renderer makes the X3 and X4 layouts cheap to verify in host tests.
struct CrossViLayout {
  Rect card;
  Rect cover;
  Rect details;
  Rect title;
  Rect stats;
  Rect progress;
  Rect progressBar;
  Rect progressLabel;
  Rect continueButton;
  Rect continueButtonWithoutProgress;

  static CrossViLayout calculate(const Rect tile) {
    constexpr int OUTER_PADDING = 20;
    constexpr int CARD_VERTICAL_PADDING = 8;
    constexpr int INNER_PADDING = 12;
    constexpr int INNER_GAP = 12;
    constexpr int COVER_LEFT_MARGIN = 8;
    constexpr int COVER_RIGHT_MARGIN = 4;
    constexpr int TITLE_HEIGHT = 58;
    constexpr int STATS_HEIGHT = 22;
    constexpr int PROGRESS_HEIGHT = 18;
    constexpr int BUTTON_HEIGHT = 38;
    constexpr int TITLE_STATS_GAP = 6;
    constexpr int STATS_PROGRESS_GAP = 12;
    constexpr int PROGRESS_BUTTON_GAP = 8;
    constexpr int NO_PROGRESS_BUTTON_GAP = 24;

    CrossViLayout out;
    out.card = Rect{tile.x + OUTER_PADDING, tile.y + CARD_VERTICAL_PADDING, std::max(0, tile.width - OUTER_PADDING * 2),
                    std::max(0, tile.height - CARD_VERTICAL_PADDING * 2)};

    const Rect inner{out.card.x + INNER_PADDING, out.card.y + INNER_PADDING,
                     std::max(0, out.card.width - INNER_PADDING * 2), std::max(0, out.card.height - INNER_PADDING * 2)};
    constexpr int preferredCoverSlotWidth = 168;
    const int maxCoverSlotWidth = std::max(0, inner.width - INNER_GAP - 160);
    const int coverSlotWidth = std::min(preferredCoverSlotWidth, maxCoverSlotWidth);
    const int coverLeftMargin = std::min(COVER_LEFT_MARGIN, coverSlotWidth);
    const int coverRightMargin = std::min(COVER_RIGHT_MARGIN, std::max(0, coverSlotWidth - coverLeftMargin));
    const int coverWidth = std::max(0, coverSlotWidth - coverLeftMargin - coverRightMargin);
    const int coverHeight = std::min(inner.height, coverWidth * 3 / 2);
    out.cover = Rect{inner.x + coverLeftMargin, inner.y + std::max(0, (inner.height - coverHeight) / 2), coverWidth,
                     coverHeight};

    const int detailsX = inner.x + coverSlotWidth + (coverSlotWidth > 0 ? INNER_GAP : 0);
    out.details = Rect{detailsX, inner.y, std::max(0, inner.x + inner.width - detailsX), inner.height};

    const int contentHeight = TITLE_HEIGHT + TITLE_STATS_GAP + STATS_HEIGHT + STATS_PROGRESS_GAP + PROGRESS_HEIGHT +
                              PROGRESS_BUTTON_GAP + BUTTON_HEIGHT;
    int y = out.details.y + std::max(0, (out.details.height - contentHeight) / 2);
    out.title = Rect{out.details.x, y, out.details.width, std::min(TITLE_HEIGHT, out.details.height)};
    y = std::min(out.details.y + out.details.height, out.title.y + out.title.height + TITLE_STATS_GAP);
    out.stats = Rect{out.details.x, y, out.details.width,
                     std::min(STATS_HEIGHT, std::max(0, out.details.y + out.details.height - y))};
    y = std::min(out.details.y + out.details.height, out.stats.y + out.stats.height + STATS_PROGRESS_GAP);
    out.progress = Rect{out.details.x, y, out.details.width,
                        std::min(PROGRESS_HEIGHT, std::max(0, out.details.y + out.details.height - y))};

    constexpr int PROGRESS_LABEL_WIDTH = 42;
    constexpr int PROGRESS_LABEL_GAP = 8;
    const int progressLabelWidth = std::min(PROGRESS_LABEL_WIDTH, out.progress.width);
    const int progressGap = out.progress.width > progressLabelWidth ? PROGRESS_LABEL_GAP : 0;
    out.progressBar = Rect{out.progress.x, out.progress.y,
                           std::max(0, out.progress.width - progressLabelWidth - progressGap), out.progress.height};
    out.progressLabel = Rect{out.progress.x + out.progress.width - progressLabelWidth, out.progress.y,
                             progressLabelWidth, out.progress.height};

    const int buttonWidth = std::min(176, out.details.width);
    y = std::min(out.details.y + out.details.height, out.progress.y + out.progress.height + PROGRESS_BUTTON_GAP);
    out.continueButton = Rect{out.details.x, y, buttonWidth,
                              std::min(BUTTON_HEIGHT, std::max(0, out.details.y + out.details.height - y))};
    const int noProgressY =
        std::min(out.details.y + out.details.height, out.stats.y + out.stats.height + NO_PROGRESS_BUTTON_GAP);
    out.continueButtonWithoutProgress =
        Rect{out.details.x, noProgressY, buttonWidth,
             std::min(BUTTON_HEIGHT, std::max(0, out.details.y + out.details.height - noProgressY))};
    return out;
  }
};

// Home layout 1 reuses the same top tile as layout 2, replacing the cover
// card with a compact recent-reading list. Capacity stays bounded so the
// menu and button hints below never move or allocate dynamically.
struct CrossViRecentListLayout {
  static constexpr int MIN_ROW_HEIGHT = 52;
  static constexpr int MAX_ROW_HEIGHT = 70;
  static constexpr int MAX_ROWS = 4;

  Rect card;
  Rect header;
  Rect list;
  int rowHeight = 0;
  int visibleRows = 0;

  static int capacity(const Rect tile) {
    constexpr int CARD_VERTICAL_PADDING = 8;
    constexpr int INNER_PADDING = 10;
    constexpr int HEADER_HEIGHT = 44;
    constexpr int HEADER_GAP = 8;
    const int cardHeight = std::max(0, tile.height - CARD_VERTICAL_PADDING * 2);
    const int available = std::max(0, cardHeight - INNER_PADDING * 2 - HEADER_HEIGHT - HEADER_GAP);
    return std::min(MAX_ROWS, available / MIN_ROW_HEIGHT);
  }

  static CrossViRecentListLayout calculate(const Rect tile, const int requestedRows) {
    constexpr int OUTER_PADDING = 20;
    constexpr int CARD_VERTICAL_PADDING = 8;
    constexpr int INNER_PADDING = 10;
    constexpr int HEADER_HEIGHT = 44;
    constexpr int HEADER_GAP = 8;

    CrossViRecentListLayout out;
    out.card = Rect{tile.x + OUTER_PADDING, tile.y + CARD_VERTICAL_PADDING, std::max(0, tile.width - OUTER_PADDING * 2),
                    std::max(0, tile.height - CARD_VERTICAL_PADDING * 2)};
    out.header = Rect{out.card.x + INNER_PADDING, out.card.y + INNER_PADDING,
                      std::max(0, out.card.width - INNER_PADDING * 2), HEADER_HEIGHT};
    const int listY = out.header.y + out.header.height + HEADER_GAP;
    const int availableHeight = std::max(0, out.card.y + out.card.height - INNER_PADDING - listY);
    out.visibleRows = std::max(0, std::min(requestedRows, capacity(tile)));
    if (out.visibleRows > 0) {
      out.rowHeight = std::min(MAX_ROW_HEIGHT, availableHeight / out.visibleRows);
      out.list = Rect{out.header.x, listY, out.header.width, out.rowHeight * out.visibleRows};
    } else {
      out.list = Rect{out.header.x, listY, out.header.width, availableHeight};
    }
    return out;
  }

  Rect row(const int index) const {
    if (index < 0 || index >= visibleRows) return Rect{};
    return Rect{list.x, list.y + index * rowHeight, list.width, rowHeight};
  }
};

// Home layout 3 presents up to three recent covers as an open shelf, without
// wrapping them in another visible card.
struct CrossViTripleCoverLayout {
  static constexpr int ITEM_COUNT = 3;

  static std::array<int, ITEM_COUNT> fixedBookIndices(const int bookCount) {
    if (bookCount <= 0) return {-1, -1, -1};
    if (bookCount == 1) return {-1, 0, -1};
    if (bookCount == 2) return {0, 1, -1};
    return {0, 1, 2};
  }

  Rect card;
  std::array<Rect, ITEM_COUNT> items;
  std::array<Rect, ITEM_COUNT> covers;
  std::array<Rect, ITEM_COUNT> titles;

  static CrossViTripleCoverLayout calculate(const Rect tile) {
    constexpr int OUTER_PADDING = 16;
    constexpr int CARD_VERTICAL_PADDING = 4;
    constexpr int INNER_PADDING = 0;
    constexpr int COLUMN_GAP = 8;
    constexpr int COVER_WIDTH = 144;
    constexpr int COVER_HEIGHT = 240;
    constexpr int TITLE_GAP = 8;
    constexpr int TITLE_HEIGHT = 44;

    CrossViTripleCoverLayout out;
    out.card = Rect{tile.x + OUTER_PADDING, tile.y + CARD_VERTICAL_PADDING, std::max(0, tile.width - OUTER_PADDING * 2),
                    std::max(0, tile.height - CARD_VERTICAL_PADDING * 2)};
    const Rect inner{out.card.x + INNER_PADDING, out.card.y + INNER_PADDING,
                     std::max(0, out.card.width - INNER_PADDING * 2), std::max(0, out.card.height - INNER_PADDING * 2)};
    const int itemWidth = std::max(0, (inner.width - COLUMN_GAP * (ITEM_COUNT - 1)) / ITEM_COUNT);
    const int coverWidth = std::min(COVER_WIDTH, itemWidth);
    const int coverHeight = std::min(COVER_HEIGHT, coverWidth * 5 / 3);
    const int groupHeight = coverHeight + TITLE_GAP + TITLE_HEIGHT;
    const int groupY = inner.y + std::max(0, (inner.height - groupHeight) / 2);

    for (int index = 0; index < ITEM_COUNT; ++index) {
      const int itemX = inner.x + index * (itemWidth + COLUMN_GAP);
      out.items[index] = Rect{itemX, inner.y, itemWidth, inner.height};
      out.covers[index] = Rect{itemX + std::max(0, (itemWidth - coverWidth) / 2), groupY, coverWidth, coverHeight};
      out.titles[index] = Rect{itemX + 2, groupY + coverHeight + TITLE_GAP, std::max(0, itemWidth - 4), TITLE_HEIGHT};
    }
    return out;
  }
};

// Pure geometry for Home layout 4. The content rect starts below CrossVi's
// existing title bar and ends above the physical-button hints.
struct CrossViCarouselLayout {
  static constexpr int COVER_PERSPECTIVE_INSET = 6;
  static constexpr int THUMBNAIL_HEIGHT = 456;

  Rect coverArea;
  Rect previousCover;
  Rect currentCover;
  Rect nextCover;
  Rect pageDots;
  Rect author;
  Rect title;
  Rect menu;

  static std::array<int, 2> sideBookIndices(int currentIndex, const int bookCount) {
    if (bookCount <= 1) return {-1, -1};
    currentIndex = std::clamp(currentIndex, 0, bookCount - 1);
    if (bookCount == 2) return {1 - currentIndex, -1};
    return {(currentIndex + bookCount - 1) % bookCount, (currentIndex + 1) % bookCount};
  }

  // Kiểu 4 keeps a two-book carousel chained in the direction of travel:
  // [current, next] becomes [previous, current] after moving right.
  static std::array<int, 2> carouselSideBookIndices(int currentIndex, const int bookCount) {
    if (bookCount <= 1) return {-1, -1};
    currentIndex = std::clamp(currentIndex, 0, bookCount - 1);
    if (bookCount == 2) return {currentIndex == 0 ? -1 : 0, currentIndex == 0 ? 1 : -1};
    return sideBookIndices(currentIndex, bookCount);
  }

  static int selectedDotIndex(const int currentIndex, const int bookCount) {
    const int dotCount = std::clamp(bookCount, 0, 3);
    return dotCount == 0 ? -1 : std::clamp(currentIndex, 0, dotCount - 1);
  }

  static int perspectiveColumnInset(const int direction, int column, const int width) {
    if (direction == 0 || width <= 1) return 0;
    column = std::max(0, std::min(column, width - 1));
    return direction > 0 ? COVER_PERSPECTIVE_INSET * column / (width - 1)
                         : COVER_PERSPECTIVE_INSET * (width - 1 - column) / (width - 1);
  }

  static Rect fitCoverWithin(const Rect frame, const int sourceWidth, const int sourceHeight) {
    if (frame.width <= 0 || frame.height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return Rect{};
    const int widthLimitedHeight = static_cast<int>(static_cast<int64_t>(frame.width) * sourceHeight / sourceWidth);
    const int drawHeight = std::max(1, std::min(frame.height, widthLimitedHeight));
    const int heightLimitedWidth = static_cast<int>(static_cast<int64_t>(drawHeight) * sourceWidth / sourceHeight);
    const int drawWidth = std::max(1, std::min(frame.width, heightLimitedWidth));
    return Rect{frame.x + (frame.width - drawWidth) / 2, frame.y + (frame.height - drawHeight) / 2, drawWidth,
                drawHeight};
  }

  static CrossViCarouselLayout calculate(const Rect content) {
    constexpr int TOP_PADDING = 8;
    constexpr int BOTTOM_PADDING = 32;
    constexpr int SIDE_PADDING = 20;
    constexpr int MENU_HEIGHT = 64;
    constexpr int TITLE_HEIGHT = 32;
    constexpr int AUTHOR_HEIGHT = 24;
    constexpr int DOTS_HEIGHT = 16;
    constexpr int TEXT_GAP = 4;
    constexpr int DOTS_TEXT_GAP = 24;
    constexpr int SECTION_GAP = 12;
    constexpr int COVER_SIDE_MARGIN = 8;
    constexpr int COVER_GAP = 8;
    constexpr int COVER_WIDTH_PERCENT = 52;
    constexpr int MAX_COVER_HEIGHT = 456;

    CrossViCarouselLayout out;
    const int contentBottom = content.y + content.height;
    out.menu = Rect{content.x + SIDE_PADDING, contentBottom - BOTTOM_PADDING - MENU_HEIGHT,
                    std::max(0, content.width - SIDE_PADDING * 2), MENU_HEIGHT};
    out.title = Rect{content.x + SIDE_PADDING, out.menu.y - SECTION_GAP - TITLE_HEIGHT,
                     std::max(0, content.width - SIDE_PADDING * 2), TITLE_HEIGHT};
    out.author = Rect{content.x + SIDE_PADDING, out.title.y - TEXT_GAP - AUTHOR_HEIGHT,
                      std::max(0, content.width - SIDE_PADDING * 2), AUTHOR_HEIGHT};
    out.pageDots = Rect{content.x, out.author.y - DOTS_TEXT_GAP - DOTS_HEIGHT, content.width, DOTS_HEIGHT};
    out.coverArea = Rect{content.x, content.y + TOP_PADDING, content.width,
                         std::max(0, out.pageDots.y - SECTION_GAP - (content.y + TOP_PADDING))};

    const int coverWidthLimit = content.width * COVER_WIDTH_PERCENT / 100;
    const int coverHeight =
        std::max(1, std::min({MAX_COVER_HEIGHT, out.coverArea.height - 12, coverWidthLimit * 5 / 3}));
    const int coverWidth = std::max(1, coverHeight * 3 / 5);
    out.currentCover =
        Rect{content.x + (content.width - coverWidth) / 2,
             out.coverArea.y + std::max(0, (out.coverArea.height - coverHeight) / 2), coverWidth, coverHeight};

    const int sideHeight = std::max(1, coverHeight * 70 / 100);
    const int sideY = out.coverArea.y + std::max(0, (out.coverArea.height - sideHeight) / 2);
    const int previousX = content.x + COVER_SIDE_MARGIN;
    out.previousCover = Rect{previousX, sideY, std::max(0, out.currentCover.x - COVER_GAP - previousX), sideHeight};
    const int currentRight = out.currentCover.x + out.currentCover.width;
    const int nextX = currentRight + COVER_GAP;
    out.nextCover = Rect{nextX, sideY, std::max(0, content.x + content.width - COVER_SIDE_MARGIN - nextX), sideHeight};
    return out;
  }
};
