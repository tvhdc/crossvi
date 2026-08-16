#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// Native X4 portrait maps XTH's right-to-left source columns directly onto
// physical landscape framebuffer rows. Compose the B/W base and the two
// controller grayscale planes byte-for-byte; outputs may alias either input.
inline void composeNativeXthPlaneBytes(const uint8_t* bit0, const uint8_t* bit1, const size_t size, uint8_t* base,
                                       uint8_t* lsb, uint8_t* msb) {
  if (!bit0 || !bit1) return;
  for (size_t index = 0; index < size; ++index) {
    const uint8_t first = bit0[index];
    const uint8_t second = bit1[index];
    if (base) base[index] = static_cast<uint8_t>(~(first | second));
    if (lsb) lsb[index] = static_cast<uint8_t>(first & ~second);
    if (msb) msb[index] = static_cast<uint8_t>(first ^ second);
  }
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

// Rotate whole eight-row strips from XTG's portrait row-major payload into a
// native landscape framebuffer. Source and destination use the same 1=white,
// 0=black polarity, so the hot path is only an 8x8 bit transpose plus row
// reversal; no per-pixel renderer calls or full-page staging buffer are needed.
inline bool rotateXtgPortraitRowsToNativeLandscape(const uint8_t* source, const size_t sourceSize,
                                                   const size_t sourceOffset, const uint16_t sourceWidth,
                                                   const uint16_t sourceHeight, uint8_t* target,
                                                   const size_t targetSize) {
  PageLayout layout;
  if (!source || !target || sourceWidth % 8U != 0 || sourceHeight % 8U != 0 ||
      !calculatePageLayout(sourceWidth, sourceHeight, 1, layout) || sourceOffset > layout.payloadBytes ||
      sourceSize > layout.payloadBytes - sourceOffset || sourceOffset % layout.rowBytes != 0 ||
      sourceSize % layout.rowBytes != 0) {
    return false;
  }

  const size_t firstSourceRow = sourceOffset / layout.rowBytes;
  const size_t sourceRowCount = sourceSize / layout.rowBytes;
  const size_t targetRowBytes = sourceHeight / 8U;
  size_t requiredTargetBytes = 0;
  if (firstSourceRow % 8U != 0 || sourceRowCount == 0 || sourceRowCount % 8U != 0 ||
      firstSourceRow + sourceRowCount > sourceHeight ||
      !checkedMultiply(targetRowBytes, sourceWidth, requiredTargetBytes) || targetSize < requiredTargetBytes) {
    return false;
  }

  for (size_t localRow = 0; localRow < sourceRowCount; localRow += 8U) {
    const size_t targetByte = (firstSourceRow + localRow) / 8U;
    for (size_t sourceByte = 0; sourceByte < layout.rowBytes; ++sourceByte) {
      for (uint8_t sourceBit = 0; sourceBit < 8U; ++sourceBit) {
        uint8_t transposed = 0;
        for (uint8_t rowBit = 0; rowBit < 8U; ++rowBit) {
          const uint8_t sourceValue = source[(localRow + rowBit) * layout.rowBytes + sourceByte];
          transposed |= static_cast<uint8_t>(((sourceValue >> (7U - sourceBit)) & 1U) << (7U - rowBit));
        }
        const size_t sourceX = sourceByte * 8U + sourceBit;
        const size_t targetRow = sourceWidth - 1U - sourceX;
        target[targetRow * targetRowBytes + targetByte] = transposed;
      }
    }
  }
  return true;
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

struct XthPortraitRows {
  uint16_t yStart = 0;
  uint16_t count = 0;
};

// Scale matching XTH column chunks straight into physical portrait rows. The
// source width/height may only shrink: that keeps every source column bounded
// to at most one output row, so callers need scratch for this chunk rather than
// a second full framebuffer. Output planes use the controller's packed polarity.
inline bool composeScaledXthPortraitRows(
    const uint8_t* bit0, const uint8_t* bit1, const size_t size, const size_t planeOffset,
    const PageLayout& layout, const uint16_t sourceWidth, const uint16_t sourceHeight, const Viewport& viewport,
    const uint16_t panelWidth, const uint16_t panelHeight, uint8_t* baseRows, uint8_t* lsbRows, uint8_t* msbRows,
    const size_t rowBufferSize, XthPortraitRows& rows) {
  rows = {};
  PageLayout expected;
  if (!bit0 || !bit1 || (!baseRows && !lsbRows && !msbRows) || size == 0 || panelWidth == 0 || panelHeight == 0 ||
      panelWidth % 8U != 0 || !calculatePageLayout(sourceWidth, sourceHeight, 2, expected) ||
      layout.columnBytes != expected.columnBytes || layout.planeBytes != expected.planeBytes ||
      layout.payloadBytes != expected.payloadBytes || viewport.width == 0 || viewport.height == 0 ||
      viewport.width > sourceWidth || viewport.height > sourceHeight ||
      static_cast<uint32_t>(viewport.x) + viewport.width > panelHeight ||
      static_cast<uint32_t>(viewport.y) + viewport.height > panelWidth || planeOffset % layout.columnBytes != 0 ||
      size % layout.columnBytes != 0 || planeOffset > layout.planeBytes || size > layout.planeBytes - planeOffset) {
    return false;
  }

  const size_t firstColumn = planeOffset / layout.columnBytes;
  const size_t columnCount = size / layout.columnBytes;
  if (firstColumn >= sourceWidth || columnCount > sourceWidth - firstColumn) return false;

  bool hasRows = false;
  uint16_t firstPhysicalRow = panelHeight;
  uint16_t lastPhysicalRow = 0;
  for (size_t localColumn = 0; localColumn < columnCount; ++localColumn) {
    const uint16_t sourceX = static_cast<uint16_t>(sourceWidth - 1U - (firstColumn + localColumn));
    const CoordinateRange destination = mapSourceCoordinateRange(sourceX, sourceWidth, viewport.width);
    for (uint16_t x = destination.begin; x < destination.end; ++x) {
      const uint16_t physicalRow = static_cast<uint16_t>(panelHeight - 1U - viewport.x - x);
      firstPhysicalRow = std::min(firstPhysicalRow, physicalRow);
      lastPhysicalRow = std::max(lastPhysicalRow, physicalRow);
      hasRows = true;
    }
  }
  if (!hasRows) return true;

  rows.yStart = firstPhysicalRow;
  rows.count = static_cast<uint16_t>(lastPhysicalRow - firstPhysicalRow + 1U);
  const size_t panelRowBytes = panelWidth / 8U;
  size_t requiredBytes = 0;
  if (!checkedMultiply(rows.count, panelRowBytes, requiredBytes) || requiredBytes > rowBufferSize) {
    rows = {};
    return false;
  }
  if (baseRows) std::memset(baseRows, 0xFF, requiredBytes);
  if (lsbRows) std::memset(lsbRows, 0x00, requiredBytes);
  if (msbRows) std::memset(msbRows, 0x00, requiredBytes);

  for (size_t localColumn = 0; localColumn < columnCount; ++localColumn) {
    const uint16_t sourceX = static_cast<uint16_t>(sourceWidth - 1U - (firstColumn + localColumn));
    const CoordinateRange destination = mapSourceCoordinateRange(sourceX, sourceWidth, viewport.width);
    const uint8_t* const firstColumnBytes = bit0 + localColumn * layout.columnBytes;
    const uint8_t* const secondColumnBytes = bit1 + localColumn * layout.columnBytes;
    for (uint16_t x = destination.begin; x < destination.end; ++x) {
      const uint16_t physicalRow = static_cast<uint16_t>(panelHeight - 1U - viewport.x - x);
      const size_t rowOffset = static_cast<size_t>(physicalRow - rows.yStart) * panelRowBytes;
      for (uint16_t y = 0; y < viewport.height; ++y) {
        const uint16_t sourceY = mapViewportCoordinate(y, viewport.height, sourceHeight);
        const uint8_t sourceMask = static_cast<uint8_t>(1U << (7U - sourceY % 8U));
        const bool first = (firstColumnBytes[sourceY / 8U] & sourceMask) != 0;
        const bool second = (secondColumnBytes[sourceY / 8U] & sourceMask) != 0;
        const uint16_t physicalX = static_cast<uint16_t>(viewport.y + y);
        const size_t byteOffset = rowOffset + physicalX / 8U;
        const uint8_t outputMask = static_cast<uint8_t>(1U << (7U - physicalX % 8U));
        if (baseRows && (first || second)) baseRows[byteOffset] &= static_cast<uint8_t>(~outputMask);
        if (lsbRows && first && !second) lsbRows[byteOffset] |= outputMask;
        if (msbRows && first != second) msbRows[byteOffset] |= outputMask;
      }
    }
  }
  return true;
}

}  // namespace xtc
