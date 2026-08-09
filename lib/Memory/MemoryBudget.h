#pragma once

#include <cstdint>

namespace MemoryBudget {

struct Snapshot {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t stackWatermark = 0;
};

struct Requirement {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
};

// Conservative failure boundaries, not performance targets. Individual
// decoders and parsers still enforce their own input and allocation limits.
inline constexpr Requirement TEXT_LAYOUT{44U * 1024U, 32U * 1024U};
inline constexpr Requirement JPEG_DECODE{36U * 1024U, 24U * 1024U};
inline constexpr Requirement PNG_DECODE{60U * 1024U, 48U * 1024U};
// A v4 BMP .cpfont can keep at most 4096 compact 6-byte intervals in one
// allocation. Leave another 8 KiB for the font object and renderer map nodes.
// Larger, file-specific allocations are made with nothrow and fail cleanly in
// SdCardFont::load(), so a blanket 48 KiB gate only rejects valid Latin fonts
// on the fragmented heap normally seen after Wi-Fi/settings use.
inline constexpr uint32_t SD_FONT_COMPACT_INTERVAL_BYTES = 4096U * 6U;
inline constexpr Requirement SD_FONT_LOAD{72U * 1024U, SD_FONT_COMPACT_INTERVAL_BYTES + 8U * 1024U};
// CrossInk stops before growing CSS selector containers at these measured
// boundaries because the firmware uses throwing STL allocators with
// exceptions disabled. Existing rules can still be merged below this limit.
inline constexpr Requirement CSS_RULE_GROWTH{64U * 1024U, 8U * 1024U};
// CrossPoint field data shows TLS handshakes succeeding with a 42 KiB largest
// block. Keep separate total and contiguous requirements so sync is not
// rejected solely because the heap is fragmented below the old 55 KiB gate.
inline constexpr Requirement KOREADER_TLS{50000U, 20000U};

inline constexpr bool hasHeadroom(const uint32_t freeHeap, const uint32_t maxAllocHeap, const Requirement requirement) {
  return freeHeap >= requirement.freeHeap && maxAllocHeap >= requirement.maxAllocHeap;
}

inline constexpr bool hasHeadroom(const Snapshot& snapshot, const Requirement requirement) {
  return hasHeadroom(snapshot.freeHeap, snapshot.maxAllocHeap, requirement);
}

inline constexpr bool hasContiguousHeadroom(const uint32_t maxAllocHeap, const uint32_t allocationBytes,
                                            const uint32_t safetyMargin) {
  return allocationBytes <= UINT32_MAX - safetyMargin && maxAllocHeap >= allocationBytes + safetyMargin;
}

Snapshot snapshot();
void logStage(const char* origin, const char* stage);

}  // namespace MemoryBudget
