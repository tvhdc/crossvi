#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ReaderFontSize {

inline constexpr std::array<uint8_t, 9> POINT_SIZES = {12, 14, 16, 18, 20, 22, 24, 26, 28};
inline constexpr uint8_t COUNT = static_cast<uint8_t>(POINT_SIZES.size());
inline constexpr uint8_t BUILTIN_COUNT = 4;
inline constexpr uint8_t DEFAULT_INDEX = 1;

constexpr bool isValidIndex(const uint8_t index) { return index < COUNT; }

constexpr uint8_t pointSize(const uint8_t index) { return POINT_SIZES[isValidIndex(index) ? index : DEFAULT_INDEX]; }

constexpr uint8_t closestIndex(const uint8_t targetPointSize, uint8_t count = COUNT) {
  if (count == 0 || count > COUNT) count = COUNT;
  uint8_t bestIndex = 0;
  uint8_t bestDelta = UINT8_MAX;
  for (uint8_t index = 0; index < count; ++index) {
    const uint8_t candidate = POINT_SIZES[index];
    const uint8_t delta = candidate > targetPointSize ? candidate - targetPointSize : targetPointSize - candidate;
    if (delta < bestDelta) {
      bestIndex = index;
      bestDelta = delta;
    }
  }
  return bestIndex;
}

}  // namespace ReaderFontSize
