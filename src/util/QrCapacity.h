#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace QrCapacity {

struct Selection {
  uint8_t version;
  size_t maxBytes;
};

// Safe byte-mode capacities for ECC_LOW. Numeric and alphanumeric payloads
// may fit more, but these limits are valid for every UTF-8 payload.
inline constexpr std::array<Selection, 5> SUPPORTED{{
    {4, 78},
    {10, 271},
    {20, 858},
    {30, 1732},
    {40, 2953},
}};

inline Selection select(const size_t bytes) {
  const auto candidate = std::find_if(SUPPORTED.begin(), SUPPORTED.end(),
                                      [bytes](const Selection& entry) { return bytes <= entry.maxBytes; });
  return candidate == SUPPORTED.end() ? SUPPORTED.back() : *candidate;
}

inline constexpr size_t maxBytes() { return SUPPORTED.back().maxBytes; }

}  // namespace QrCapacity
