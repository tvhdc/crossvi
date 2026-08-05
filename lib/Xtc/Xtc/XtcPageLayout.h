#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace xtc {

struct PageLayout {
  size_t rowBytes = 0;
  size_t columnBytes = 0;
  size_t planeBytes = 0;
  size_t payloadBytes = 0;
};

inline bool checkedAdd(const size_t left, const size_t right, size_t& result) {
  if (right > std::numeric_limits<size_t>::max() - left) return false;
  result = left + right;
  return true;
}

inline bool checkedMultiply(const size_t left, const size_t right, size_t& result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
  result = left * right;
  return true;
}

// XTG is row-major. XTH stores two column-major bit planes, so rounding must
// happen on the height of each column before multiplying by the width.
inline bool calculatePageLayout(const uint16_t width, const uint16_t height, const uint8_t bitDepth,
                                PageLayout& layout) {
  layout = {};
  if (width == 0 || height == 0 || (bitDepth != 1 && bitDepth != 2)) return false;

  if (bitDepth == 1) {
    layout.rowBytes = (static_cast<size_t>(width) + 7U) / 8U;
    return checkedMultiply(layout.rowBytes, height, layout.payloadBytes);
  }

  layout.columnBytes = (static_cast<size_t>(height) + 7U) / 8U;
  if (!checkedMultiply(width, layout.columnBytes, layout.planeBytes)) return false;
  return checkedMultiply(layout.planeBytes, 2U, layout.payloadBytes);
}

inline uint8_t readXthPixel(const uint8_t* payload, const PageLayout& layout, const uint16_t width, const uint16_t x,
                            const uint16_t y) {
  const size_t column = static_cast<size_t>(width - 1U - x);
  const size_t byteOffset = column * layout.columnBytes + y / 8U;
  const uint8_t bit = static_cast<uint8_t>(7U - y % 8U);
  const uint8_t bit0 = static_cast<uint8_t>((payload[byteOffset] >> bit) & 1U);
  const uint8_t bit1 = static_cast<uint8_t>((payload[layout.planeBytes + byteOffset] >> bit) & 1U);
  return static_cast<uint8_t>(bit0 | (bit1 << 1U));
}

struct Viewport {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct CoordinateRange {
  uint16_t begin = 0;
  uint16_t end = 0;
};

inline bool calculateFitViewport(const uint16_t sourceWidth, const uint16_t sourceHeight, const uint16_t screenWidth,
                                 const uint16_t screenHeight, Viewport& viewport) {
  viewport = {};
  if (sourceWidth == 0 || sourceHeight == 0 || screenWidth == 0 || screenHeight == 0) return false;

  uint32_t targetWidth = screenWidth;
  uint32_t targetHeight = screenHeight;
  if (static_cast<uint64_t>(screenWidth) * sourceHeight <= static_cast<uint64_t>(screenHeight) * sourceWidth) {
    targetHeight = static_cast<uint32_t>((static_cast<uint64_t>(sourceHeight) * screenWidth) / sourceWidth);
  } else {
    targetWidth = static_cast<uint32_t>((static_cast<uint64_t>(sourceWidth) * screenHeight) / sourceHeight);
  }
  if (targetWidth == 0 || targetHeight == 0) return false;

  viewport.x = static_cast<uint16_t>((screenWidth - targetWidth) / 2U);
  viewport.y = static_cast<uint16_t>((screenHeight - targetHeight) / 2U);
  viewport.width = static_cast<uint16_t>(targetWidth);
  viewport.height = static_cast<uint16_t>(targetHeight);
  return true;
}

inline uint16_t mapViewportCoordinate(const uint16_t destination, const uint16_t destinationSize,
                                      const uint16_t sourceSize) {
  if (destinationSize <= 1 || sourceSize <= 1) return 0;
  return static_cast<uint16_t>((static_cast<uint64_t>(destination) * (sourceSize - 1U)) / (destinationSize - 1U));
}

// Inverse of mapViewportCoordinate(). The half-open result contains every
// destination coordinate which samples this source coordinate. It lets the
// XTCH reader keep the exact fit-to-screen behavior while processing source
// pixels as small streamed chunks.
inline CoordinateRange mapSourceCoordinateRange(const uint16_t source, const uint16_t sourceSize,
                                                const uint16_t destinationSize) {
  if (source >= sourceSize || sourceSize == 0 || destinationSize == 0) return {};
  if (sourceSize == 1) return source == 0 ? CoordinateRange{0, destinationSize} : CoordinateRange{};
  if (destinationSize == 1) return source == 0 ? CoordinateRange{0, 1} : CoordinateRange{};

  const uint64_t sourceSpan = sourceSize - 1U;
  const uint64_t destinationSpan = destinationSize - 1U;
  const auto ceilDivide = [](const uint64_t numerator, const uint64_t denominator) {
    return (numerator + denominator - 1U) / denominator;
  };
  const uint64_t begin = ceilDivide(static_cast<uint64_t>(source) * destinationSpan, sourceSpan);
  const uint64_t end = std::min<uint64_t>(
      destinationSize, ceilDivide((static_cast<uint64_t>(source) + 1U) * destinationSpan, sourceSpan));
  return {static_cast<uint16_t>(std::min<uint64_t>(begin, destinationSize)), static_cast<uint16_t>(end)};
}

// Resolve one byte from a streamed XTH payload. yBase is the first source row
// covered by the byte; its eight pixels are stored MSB-first.
inline bool locateXthStreamByte(const PageLayout& layout, const uint16_t width, const uint16_t height,
                                const size_t absoluteOffset, bool& secondPlane, uint16_t& x, uint16_t& yBase) {
  if (width == 0 || height == 0 || layout.columnBytes == 0 || layout.planeBytes == 0 ||
      absoluteOffset >= layout.payloadBytes) {
    return false;
  }
  secondPlane = absoluteOffset >= layout.planeBytes;
  const size_t planeOffset = secondPlane ? absoluteOffset - layout.planeBytes : absoluteOffset;
  const size_t column = planeOffset / layout.columnBytes;
  const size_t rowByte = planeOffset % layout.columnBytes;
  if (column >= width || rowByte * 8U >= height) return false;
  x = static_cast<uint16_t>(width - 1U - column);
  yBase = static_cast<uint16_t>(rowByte * 8U);
  return true;
}

}  // namespace xtc
