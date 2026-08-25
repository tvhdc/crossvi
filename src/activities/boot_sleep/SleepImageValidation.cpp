#include "SleepImageValidation.h"

#include <HalStorage.h>

#include <climits>
#include <limits>

#include "Bitmap.h"
#include "Epub/converters/PngToFramebufferConverter.h"

namespace SleepImageValidation {
namespace {

bool readLE16(HalFile& file, uint16_t& value) {
  uint8_t raw[2];
  if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  value = static_cast<uint16_t>(raw[0]) | static_cast<uint16_t>(raw[1]) << 8U;
  return true;
}

bool readLE32(HalFile& file, uint32_t& value) {
  uint8_t raw[4];
  if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  value = static_cast<uint32_t>(raw[0]) | static_cast<uint32_t>(raw[1]) << 8U | static_cast<uint32_t>(raw[2]) << 16U |
          static_cast<uint32_t>(raw[3]) << 24U;
  return true;
}

}  // namespace

Bmp32HeaderStatus readBmp32Header(HalFile& file, Bmp32Header& out) {
  if (!file.seek(0)) return Bmp32HeaderStatus::Invalid;
  const uint64_t fileSize = file.fileSize64();
  uint16_t bfType = 0;
  uint32_t declaredFileSize = 0;
  uint32_t pixelOffset = 0;
  if (!readLE16(file, bfType) || !readLE32(file, declaredFileSize) || !file.seekCur(4) ||
      !readLE32(file, pixelOffset) || bfType != 0x4D42) {
    return Bmp32HeaderStatus::Invalid;
  }

  uint32_t dibSize = 0;
  uint32_t rawWidth = 0;
  uint32_t rawHeightBits = 0;
  uint16_t planes = 0;
  uint16_t bpp = 0;
  uint32_t compression = 0;
  if (!readLE32(file, dibSize) || dibSize < 40 || !readLE32(file, rawWidth) || !readLE32(file, rawHeightBits) ||
      !readLE16(file, planes) || !readLE16(file, bpp) || !readLE32(file, compression)) {
    return Bmp32HeaderStatus::Invalid;
  }
  if (bpp != 32) return Bmp32HeaderStatus::Not32Bit;

  const int32_t width = static_cast<int32_t>(rawWidth);
  const int32_t rawHeight = static_cast<int32_t>(rawHeightBits);
  if (width <= 0 || rawHeight == INT32_MIN || planes != 1) return Bmp32HeaderStatus::Invalid;
  const int32_t height = rawHeight < 0 ? -rawHeight : rawHeight;
  constexpr int MAX_OVERLAY_WIDTH = 2048;
  constexpr int MAX_OVERLAY_HEIGHT = 3072;
  if (height <= 0 || width > MAX_OVERLAY_WIDTH || height > MAX_OVERLAY_HEIGHT ||
      !(compression == 0 || compression == 3)) {
    return Bmp32HeaderStatus::Invalid;
  }

  if (compression == 3) {
    if (!file.seek64(14ULL + 40ULL)) return Bmp32HeaderStatus::Invalid;
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;
    if (!readLE32(file, redMask) || !readLE32(file, greenMask) || !readLE32(file, blueMask) ||
        !readLE32(file, alphaMask) || redMask != 0x00FF0000UL || greenMask != 0x0000FF00UL ||
        blueMask != 0x000000FFUL || alphaMask != 0xFF000000UL) {
      return Bmp32HeaderStatus::Invalid;
    }
  }

  const uint64_t minimumPixelOffset = 14ULL + dibSize + (compression == 3 && dibSize == 40 ? 16ULL : 0ULL);
  const uint64_t rowBytes = static_cast<uint64_t>(width) * 4ULL;
  const uint64_t pixelBytes = rowBytes * static_cast<uint64_t>(height);
  if (pixelOffset < minimumPixelOffset || rowBytes > std::numeric_limits<uint32_t>::max() || pixelOffset > fileSize ||
      pixelBytes > fileSize - pixelOffset ||
      (declaredFileSize != 0 && (declaredFileSize < pixelOffset || declaredFileSize > fileSize ||
                                 pixelBytes > declaredFileSize - pixelOffset))) {
    return Bmp32HeaderStatus::Invalid;
  }

  out.width = width;
  out.height = height;
  out.topDown = rawHeight < 0;
  out.pixelOffset = pixelOffset;
  out.rowBytes = static_cast<uint32_t>(rowBytes);
  return Bmp32HeaderStatus::Valid;
}

bool normalBmp(const char* path) {
  HalFile file;
  if (!path || !Storage.openFileForRead("SLP", path, file)) return false;
  Bitmap bitmap(file, true);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
  return file.close() && valid;
}

bool overlayBmp(const char* path) {
  HalFile file;
  if (!path || !Storage.openFileForRead("SLP", path, file)) return false;
  Bmp32Header alphaHeader;
  const Bmp32HeaderStatus alphaStatus = readBmp32Header(file, alphaHeader);
  if (alphaStatus == Bmp32HeaderStatus::Valid) return file.close();
  if (alphaStatus == Bmp32HeaderStatus::Invalid || !file.seek(0)) return file.close() && false;
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
  return file.close() && valid;
}

bool overlayPng(const char* path) {
  if (!path) return false;
  ImageDimensions dimensions{};
  return PngToFramebufferConverter::getSupportedDimensionsStatic(path, dimensions) && dimensions.width > 0 &&
         dimensions.height > 0;
}

}  // namespace SleepImageValidation
