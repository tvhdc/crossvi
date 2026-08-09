#include "ReadingCalendarRenderer.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "ReadingCalendarLayout.h"
#include "fontIds.h"

namespace {
void fillHeatmapCell(const GfxRenderer& renderer, const Rect& rect, const ReadingHeatmapLevel level) {
  if (rect.width <= 0 || rect.height <= 0) return;
  switch (level) {
    case ReadingHeatmapLevel::None:
      return;
    case ReadingHeatmapLevel::Minutes15:
      for (int y = rect.y; y < rect.y + rect.height; y += 4) {
        const int offset = (y / 4 & 1) * 2;
        for (int x = rect.x + offset; x < rect.x + rect.width; x += 4) renderer.drawPixel(x, y);
      }
      return;
    case ReadingHeatmapLevel::Minutes30:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      return;
    case ReadingHeatmapLevel::Minutes60:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::DarkGray);
      return;
    case ReadingHeatmapLevel::Minutes120:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height);
      for (int y = rect.y; y < rect.y + rect.height; y += 2) {
        for (int x = rect.x + (y & 2) / 2; x < rect.x + rect.width; x += 2) renderer.drawPixel(x, y, false);
      }
      return;
    case ReadingHeatmapLevel::Minutes240:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height);
      return;
  }
}

bool usesWhiteDayNumber(const ReadingHeatmapLevel level) {
  return level == ReadingHeatmapLevel::Minutes120 || level == ReadingHeatmapLevel::Minutes240;
}
}  // namespace

namespace ReadingCalendarRenderer {
std::string formatDuration(const uint32_t seconds, const bool minimum) {
  char duration[28];
  if (seconds == 0) {
    snprintf(duration, sizeof(duration), "0m");
  } else if (seconds < 60) {
    snprintf(duration, sizeof(duration), "<1m");
  } else if (seconds < 3600) {
    snprintf(duration, sizeof(duration), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(duration, sizeof(duration), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  }
  return minimum ? std::string(tr(STR_STATS_MINIMUM_PREFIX)) + " " + duration : duration;
}

Rect drawGrid(const GfxRenderer& renderer, const Rect& bounds, const ReadingCalendarModel& model,
              const bool showSelection) {
  const ReadingCalendarGridLayout layout = ReadingCalendarGridLayout::calculate(bounds, 0);
  if (layout.cellSize <= 0) return Rect{};
  for (size_t index = 0; index < ReadingCalendarGridLayout::CELL_COUNT; ++index) {
    const ReadingCalendarCell cell = model.cellAt(index);
    if (!cell.date.isValid()) continue;
    const Rect box = layout.cell(index);
    const ReadingHeatmapLevel level =
        cell.exactDuration ? readingHeatmapLevel(cell.readingSeconds) : ReadingHeatmapLevel::None;
    const Rect fill{box.x + 1, box.y + 1, std::max(0, box.width - 2), std::max(0, box.height - 2)};
    fillHeatmapCell(renderer, fill, level);
    renderer.drawRect(box.x, box.y, box.width, box.height, showSelection && cell.selected ? 2 : 1, true);

    char day[4];
    snprintf(day, sizeof(day), "%u", static_cast<unsigned>(cell.date.day));
    const bool blackText = !usesWhiteDayNumber(level);
    renderer.drawText(SMALL_FONT_ID, box.x + 4, box.y + 3, day, blackText, EpdFontFamily::BOLD);

    if (cell.today) {
      renderer.fillRect(box.x + box.width - 8, box.y + 4, 4, 4, blackText);
    } else if (cell.legacyDuration) {
      renderer.drawLine(box.x + 5, box.y + box.height - 5, box.x + box.width - 6, box.y + box.height - 5, blackText);
    } else if (cell.exactDuration && cell.readingSeconds > 0 && level == ReadingHeatmapLevel::None) {
      renderer.fillRect(box.x + box.width / 2 - 2, box.y + box.height - 7, 4, 3, blackText);
    }
  }
  return layout.grid;
}

void drawLegend(const GfxRenderer& renderer, const Rect& bounds) {
  constexpr std::array<StrId, 5> labels = {StrId::STR_STATS_INTENSITY_15, StrId::STR_STATS_INTENSITY_30,
                                           StrId::STR_STATS_INTENSITY_60, StrId::STR_STATS_INTENSITY_120,
                                           StrId::STR_STATS_INTENSITY_240};
  constexpr std::array<ReadingHeatmapLevel, 5> levels = {
      ReadingHeatmapLevel::Minutes15, ReadingHeatmapLevel::Minutes30, ReadingHeatmapLevel::Minutes60,
      ReadingHeatmapLevel::Minutes120, ReadingHeatmapLevel::Minutes240};
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int markerSize = std::max(7, std::min(12, lineHeight - 2));
  for (size_t index = 0; index < labels.size(); ++index) {
    const char* label = I18N.get(labels[index]);
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int groupWidth = markerSize + 3 + labelWidth;
    const int anchor =
        bounds.x + static_cast<int>(index) * std::max(0, bounds.width - 1) / static_cast<int>(labels.size() - 1);
    const int groupX = index == 0                   ? bounds.x
                       : index + 1 == labels.size() ? bounds.x + bounds.width - groupWidth
                                                    : anchor - groupWidth / 2;
    const Rect marker{groupX, bounds.y + std::max(0, (lineHeight - markerSize) / 2), markerSize, markerSize};
    fillHeatmapCell(renderer, Rect{marker.x + 1, marker.y + 1, marker.width - 2, marker.height - 2}, levels[index]);
    renderer.drawRect(marker.x, marker.y, marker.width, marker.height);
    renderer.drawText(SMALL_FONT_ID, marker.x + marker.width + 3, bounds.y, label);
  }
}
}  // namespace ReadingCalendarRenderer
