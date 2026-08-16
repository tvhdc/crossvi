#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace TxtPageIndex {

// A target that falls exactly on a page boundary belongs to the following
// page, so indexing must continue until the parsed boundary is strictly past
// the target (or reaches EOF).
inline bool reachedTargetPage(const size_t parsedThrough, const size_t fileSize, const size_t targetOffset) {
  return parsedThrough >= fileSize || parsedThrough > targetOffset;
}

inline bool containsTarget(const uint32_t* offsets, const size_t count, const bool complete,
                           const uint32_t targetOffset) {
  return complete || (offsets != nullptr && count > 0 && offsets[count - 1] > targetOffset);
}

// A partial index contains one unparsed look-ahead page start. Estimate the
// final page count from the completed pages without ever reporting fewer than
// the page starts already known or more than the bounded index can hold.
inline size_t estimateTotalPages(const uint32_t* offsets, const size_t count, const uint32_t fileSize,
                                 const size_t maxPages) {
  if (!offsets || count < 2 || fileSize == 0 || maxPages == 0) return std::min(count, maxPages);

  const uint64_t parsedThrough = offsets[count - 1];
  if (parsedThrough == 0) return std::min(count, maxPages);

  const uint64_t completedPages = count - 1;
  const uint64_t estimate = (static_cast<uint64_t>(fileSize) * completedPages + parsedThrough - 1) / parsedThrough;
  return static_cast<size_t>(std::min<uint64_t>(maxPages, std::max<uint64_t>(count, estimate)));
}

}  // namespace TxtPageIndex
