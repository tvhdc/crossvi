#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

struct LibraryGridShape {
  uint8_t columns;
  uint8_t rows;
};

class LibraryGridModel final {
 public:
  static constexpr uint8_t LAYOUT_VERSION = 2;
  static constexpr size_t LIST_PAGE_SIZE = 9;

  static constexpr LibraryGridShape shape(const uint8_t /*setting*/) { return {3, 2}; }

  static constexpr bool usesPerCoverTitles(const uint8_t /*setting*/) { return true; }

  static constexpr uint8_t canonicalSetting(const int /*setting*/) { return 0; }

  static constexpr uint8_t migrateLegacySetting(const int /*setting*/) { return 0; }

  static constexpr size_t pageSize(const uint8_t setting) {
    const LibraryGridShape grid = shape(setting);
    return static_cast<size_t>(grid.columns) * grid.rows;
  }

  static constexpr size_t coverQueueOffset(const size_t cursor, const size_t selectedOffset) {
    if (cursor == 0) return selectedOffset;
    const size_t offset = cursor - 1;
    return offset >= selectedOffset ? offset + 1 : offset;
  }

  static constexpr bool coverWorkIdle(const uint32_t now, const uint32_t lastInputAt, const uint32_t idleMs) {
    return static_cast<uint32_t>(now - lastInputAt) >= idleMs;
  }

  static bool collectVisiblePageSources(const std::span<const size_t> sortedSources,
                                        const std::span<const size_t> pinnedSources, const size_t sourceCount,
                                        const size_t visibleStart, const std::span<size_t> output) {
    size_t written = 0;
    size_t pinnedIndex = std::min(visibleStart, pinnedSources.size());
    while (written < output.size() && pinnedIndex < pinnedSources.size()) {
      const size_t source = pinnedSources[pinnedIndex++];
      if (source >= sourceCount) return false;
      output[written++] = source;
    }

    size_t nonPinnedToSkip = visibleStart > pinnedSources.size() ? visibleStart - pinnedSources.size() : 0;
    for (const size_t source : sortedSources) {
      if (written == output.size()) break;
      if (source >= sourceCount ||
          std::find(pinnedSources.begin(), pinnedSources.end(), source) != pinnedSources.end()) {
        continue;
      }
      if (nonPinnedToSkip > 0) {
        --nonPinnedToSkip;
        continue;
      }
      output[written++] = source;
    }
    return written == output.size();
  }

  static constexpr int productionThumbnailWidth(const int height) { return height > 0 ? height * 3 / 5 : 0; }

  static constexpr size_t clampIndex(const size_t index, const size_t itemCount) {
    return itemCount == 0 ? 0 : std::min(index, itemCount - 1);
  }

  static constexpr size_t pageCount(const size_t itemCount, const size_t pageCapacity) {
    return itemCount == 0 || pageCapacity == 0 ? 0 : 1 + (itemCount - 1) / pageCapacity;
  }

  static constexpr size_t pageNumber(const size_t index, const size_t itemCount, const size_t pageCapacity) {
    return itemCount == 0 || pageCapacity == 0 ? 0 : clampIndex(index, itemCount) / pageCapacity + 1;
  }

  static constexpr size_t pageStart(const size_t index, const size_t itemCount, const size_t pageCapacity) {
    return itemCount == 0 || pageCapacity == 0 ? 0 : (clampIndex(index, itemCount) / pageCapacity) * pageCapacity;
  }

  static constexpr size_t lastIndexOnPage(const size_t index, const size_t itemCount, const size_t pageCapacity) {
    if (itemCount == 0 || pageCapacity == 0) return 0;
    const size_t start = pageStart(index, itemCount, pageCapacity);
    return std::min(start + pageCapacity, itemCount) - 1;
  }

  static constexpr size_t nextIndex(const size_t index, const size_t itemCount) {
    if (itemCount == 0) return 0;
    const size_t current = clampIndex(index, itemCount);
    return current + 1 < itemCount ? current + 1 : current;
  }

  static constexpr size_t previousIndex(const size_t index, const size_t itemCount) {
    if (itemCount == 0) return 0;
    const size_t current = clampIndex(index, itemCount);
    return current == 0 ? 0 : current - 1;
  }

  template <typename MatchesRememberedBook>
  static size_t restoreIndex(const size_t rememberedIndex, const size_t itemCount,
                             MatchesRememberedBook&& matchesRememberedBook) {
    if (itemCount == 0) return 0;
    const size_t clamped = clampIndex(rememberedIndex, itemCount);
    if (matchesRememberedBook(clamped)) return clamped;
    for (size_t index = 0; index < itemCount; ++index) {
      if (index != clamped && matchesRememberedBook(index)) return index;
    }
    return clamped;
  }
};
