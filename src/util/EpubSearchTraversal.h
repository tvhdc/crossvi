#pragma once

#include <algorithm>
#include <cstdint>

namespace EpubSearchTraversal {

inline constexpr bool needsBuild(const bool cacheLoaded, const bool cachePartial) {
  return !cacheLoaded || cachePartial;
}

inline constexpr int clampPage(const int requestedPage, const uint16_t pageCount) {
  if (pageCount == 0) return 0;
  return std::min(std::max(0, requestedPage), static_cast<int>(pageCount) - 1);
}

}  // namespace EpubSearchTraversal
