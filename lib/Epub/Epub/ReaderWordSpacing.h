#pragma once

#include <algorithm>
#include <cstdint>

inline int readerWordSpacingExtra(const int naturalGap, const uint8_t level) {
  if (naturalGap <= 0 || level == 0) return 0;
  return static_cast<int>(std::min<uint8_t>(level, 4)) * 10;
}
