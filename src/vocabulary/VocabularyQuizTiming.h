#pragma once

#include <cstdint>

namespace crossvi::vocabulary {

constexpr uint8_t countdownSegments(const uint32_t elapsedMs, const uint32_t durationMs,
                                    const uint8_t segmentCount = 5) {
  if (durationMs == 0 || segmentCount == 0) return segmentCount;
  if (elapsedMs >= durationMs) return 0;
  const uint64_t remaining = static_cast<uint64_t>(durationMs - elapsedMs) * segmentCount;
  return static_cast<uint8_t>((remaining + durationMs - 1) / durationMs);
}

}  // namespace crossvi::vocabulary
