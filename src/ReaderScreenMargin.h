#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ReaderScreenMargin {

inline constexpr std::array<uint8_t, 10> VALUES = {5, 10, 15, 20, 25, 30, 40, 50, 60, 70};
inline constexpr uint8_t COUNT = static_cast<uint8_t>(VALUES.size());

inline bool isValid(const uint8_t value) {
  return std::any_of(VALUES.begin(), VALUES.end(), [value](const uint8_t candidate) { return candidate == value; });
}

constexpr uint8_t closestIndex(const int value) {
  uint8_t bestIndex = 0;
  const int64_t wideValue = value;
  int64_t bestDelta = wideValue >= VALUES[0] ? wideValue - VALUES[0] : VALUES[0] - wideValue;
  for (uint8_t index = 1; index < COUNT; ++index) {
    const int64_t candidate = VALUES[index];
    const int64_t delta = wideValue >= candidate ? wideValue - candidate : candidate - wideValue;
    if (delta < bestDelta) {
      bestIndex = index;
      bestDelta = delta;
    }
  }
  return bestIndex;
}

constexpr uint8_t valueAt(const uint8_t index) { return index < COUNT ? VALUES[index] : VALUES[0]; }

constexpr uint8_t closestValue(const int value) { return valueAt(closestIndex(value)); }

}  // namespace ReaderScreenMargin
