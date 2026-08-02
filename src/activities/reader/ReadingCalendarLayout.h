#pragma once

#include <algorithm>
#include <cstddef>

#include "components/themes/BaseTheme.h"

struct ReadingCalendarGridLayout {
  static constexpr size_t CELL_COUNT = 42;
  static constexpr int COLUMNS = 7;
  static constexpr int ROWS = 6;
  static constexpr int CELL_GAP = 3;

  Rect weekdays;
  Rect grid;
  int cellSize = 0;

  static ReadingCalendarGridLayout calculate(const Rect bounds, const int weekdayHeight) {
    ReadingCalendarGridLayout layout;
    const int labelHeight = std::max(0, std::min(weekdayHeight, bounds.height));
    const int availableHeight = std::max(0, bounds.height - labelHeight);
    const int horizontalGaps = CELL_GAP * (COLUMNS - 1);
    const int verticalGaps = CELL_GAP * (ROWS - 1);
    const int cellWidth = std::max(0, bounds.width - horizontalGaps) / COLUMNS;
    const int cellHeight = std::max(0, availableHeight - verticalGaps) / ROWS;
    layout.cellSize = std::max(0, std::min(cellWidth, cellHeight));
    const int gridWidth = layout.cellSize > 0 ? layout.cellSize * COLUMNS + horizontalGaps : 0;
    const int gridHeight = layout.cellSize > 0 ? layout.cellSize * ROWS + verticalGaps : 0;
    const int gridX = bounds.x + std::max(0, (bounds.width - gridWidth) / 2);
    const int gridY = bounds.y + labelHeight;
    layout.weekdays = Rect{gridX, gridY - labelHeight, gridWidth, labelHeight};
    layout.grid = Rect{gridX, gridY, gridWidth, gridHeight};
    return layout;
  }

  Rect cell(const size_t index) const {
    if (cellSize <= 0 || index >= CELL_COUNT) return Rect{};
    return Rect{grid.x + static_cast<int>(index % COLUMNS) * (cellSize + CELL_GAP),
                grid.y + static_cast<int>(index / COLUMNS) * (cellSize + CELL_GAP), cellSize, cellSize};
  }
};
