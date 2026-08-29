#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "LibraryCatalogStore.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

class LibraryGridView final {
 public:
  static size_t pageSize(uint8_t gridSetting);
  // Draw only the page content that is independent of the focused book.  The
  // activity can snapshot this bounded region and restore it while focus
  // moves, avoiding another SD/BMP read for every key press.
  static uint8_t drawStatic(const GfxRenderer& renderer, Rect rect, const std::vector<LibraryBookRecord>& books,
                            uint8_t gridSetting, bool renderCovers = true);
  // Draw one cover cell after the inexpensive placeholder pass.  The caller
  // can schedule these cells over multiple e-ink refreshes instead of
  // blocking one render on every SD/BMP read in the page.
  static void drawCoverAt(const GfxRenderer& renderer, Rect rect, const std::vector<LibraryBookRecord>& books,
                          size_t index, uint8_t gridSetting);
  static void drawSelection(const GfxRenderer& renderer, Rect rect, const std::vector<LibraryBookRecord>& books,
                            size_t selectedOnPage, uint8_t gridSetting);
  static void draw(const GfxRenderer& renderer, Rect rect, const std::vector<LibraryBookRecord>& books,
                   size_t selectedOnPage, uint8_t gridSetting);
};
