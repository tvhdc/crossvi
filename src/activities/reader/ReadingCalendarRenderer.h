#pragma once

#include <cstdint>
#include <string>

#include "ReadingCalendarModel.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace ReadingCalendarRenderer {
std::string formatDuration(uint32_t seconds, bool minimum = false);
Rect drawGrid(const GfxRenderer& renderer, const Rect& bounds, const ReadingCalendarModel& model,
              bool showSelection = true);
void drawLegend(const GfxRenderer& renderer, const Rect& bounds);
}  // namespace ReadingCalendarRenderer
