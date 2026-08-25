#pragma once

#include <EpdFontData.h>
#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace TxtLineWrap {

inline bool isContinuationByte(const char byte) { return (static_cast<uint8_t>(byte) & 0xC0) == 0x80; }

inline size_t leadingUtf8BomBytes(const uint8_t* data, const size_t size) {
  return size >= 3 && data && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3U : 0U;
}

// SD-card fonts measure these codepoints as a sum of unsigned advances. Keeping
// this guard to printable ASCII and CJK avoids bidi shaping, combining marks,
// and font ligatures, making prefix widths monotonic and safe to binary-search.
inline bool isMonotonicLtrText(const std::string& text) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const end = cursor + text.size();
  bool hasText = false;

  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (utf8IsCjkBreakable(cp)) {
      hasText = true;
    } else if (cp >= 0x20 && cp <= 0x7E) {
      hasText = true;
    } else {
      return false;
    }
  }

  return hasText && cursor == end;
}

inline size_t nextUtf8Boundary(const std::string& text, const size_t boundary) {
  size_t next = boundary + 1;
  while (next < text.size() && isContinuationByte(text[next])) {
    ++next;
  }
  return next;
}

// Returns the largest UTF-8 prefix accepted by measure(). The full string must
// already be known not to fit, and measured prefix widths must be monotonic.
// A temporary NUL avoids allocating a substring for every probe.
template <typename Measure>
size_t findLargestFittingPrefix(std::string& text, const int maxWidth, Measure measure) {
  size_t fits = 0;
  size_t overflows = text.size();

  while (fits < overflows) {
    size_t probe = fits + (overflows - fits) / 2;
    while (probe > fits && isContinuationByte(text[probe])) {
      --probe;
    }

    if (probe == fits) {
      probe = nextUtf8Boundary(text, fits);
      if (probe >= overflows) {
        break;
      }
    }

    const char saved = text[probe];
    text[probe] = '\0';
    const int width = measure(text.c_str());
    text[probe] = saved;

    if (width <= maxWidth) {
      fits = probe;
    } else {
      overflows = probe;
    }
  }

  return fits;
}

inline size_t preserveWordBreak(const std::string& text, const size_t largestFittingPrefix) {
  const size_t space = text.rfind(' ', largestFittingPrefix);
  return space != std::string::npos && space > 0 ? space : largestFittingPrefix;
}

// Finds the same break as the legacy backward probe for simple LTR text, but
// visits every UTF-8 boundary once. The callbacks expose the built-in font's
// 12.4 advance, 4.4 kerning, and pair-ligature data. Tracking the last two
// shaped glyphs keeps differential rounding and greedy ligatures identical to
// GfxRenderer::getTextAdvanceX(), even when prefix widths are not monotonic.
template <typename Advance, typename Kerning, typename Ligature>
size_t findLargestFittingShapedLineBreak(const std::string& text, const int maxWidth, Advance advance, Kerning kerning,
                                         Ligature ligature) {
  const auto* const begin = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* cursor = begin;

  uint32_t previousCp = 0;
  uint32_t currentCp = 0;
  int32_t previousAdvance = 0;
  int32_t currentAdvance = 0;
  int widthBeforePrevious = 0;
  int widthBeforeCurrent = 0;
  int prefixWidth = 0;

  size_t largestFittingPrefix = 0;
  size_t largestFittingPrefixBeforeFirstSpace = 0;
  size_t firstSpace = std::string::npos;
  size_t largestFittingSpace = std::string::npos;

  while (*cursor) {
    const size_t bytePosition = static_cast<size_t>(cursor - begin);
    const uint32_t cp = utf8NextCodepoint(&cursor);

    // The old search probes spaces from right to left and excludes the space
    // itself. Record those candidates before appending this codepoint.
    if (cp == ' ' && bytePosition > 0) {
      if (firstSpace == std::string::npos) firstSpace = bytePosition;
      if (prefixWidth <= maxWidth) largestFittingSpace = bytePosition;
    }

    if (currentCp == 0) {
      currentCp = cp;
      currentAdvance = static_cast<int32_t>(advance(cp));
    } else if (const uint32_t joined = ligature(currentCp, cp); joined != 0) {
      currentCp = joined;
      currentAdvance = static_cast<int32_t>(advance(joined));
      widthBeforeCurrent =
          previousCp == 0 ? 0 : widthBeforePrevious + fp4::toPixel(previousAdvance + kerning(previousCp, currentCp));
    } else {
      widthBeforePrevious = widthBeforeCurrent;
      previousCp = currentCp;
      previousAdvance = currentAdvance;
      currentCp = cp;
      currentAdvance = static_cast<int32_t>(advance(cp));
      widthBeforeCurrent = widthBeforePrevious + fp4::toPixel(previousAdvance + kerning(previousCp, currentCp));
    }

    prefixWidth = widthBeforeCurrent + fp4::toPixel(currentAdvance);
    if (prefixWidth <= maxWidth) {
      largestFittingPrefix = static_cast<size_t>(cursor - begin);
    }
    if (firstSpace == std::string::npos) {
      largestFittingPrefixBeforeFirstSpace = largestFittingPrefix;
    }
  }

  if (largestFittingSpace != std::string::npos) return largestFittingSpace;
  if (firstSpace != std::string::npos) return largestFittingPrefixBeforeFirstSpace;
  return largestFittingPrefix;
}

}  // namespace TxtLineWrap
