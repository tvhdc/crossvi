#include "Bitmap.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>

// ============================================================================
// IMAGE PROCESSING OPTIONS
// ============================================================================
// Dithering is applied when converting high-color BMPs to the display's native
// 2-bit (4-level) grayscale. Images whose palette entries all map to native
// gray levels (0, 85, 170, 255 ±21) are mapped directly without dithering.
// For cover images, dithering is done in JpegToBmpConverter.cpp instead.
constexpr bool USE_ATKINSON = true;  // Use Atkinson dithering instead of Floyd-Steinberg
// ============================================================================

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;

  delete atkinsonDitherer;
  delete fsDitherer;
}

BitmapFileStatus Bitmap::inspectFile(const char* path) {
  if (!path) return BitmapFileStatus::Invalid;
  HalFile candidate;
  if (!Storage.openFileForRead("BMP", path, candidate)) {
    return Storage.exists(path) ? BitmapFileStatus::IoError : BitmapFileStatus::Missing;
  }
  Bitmap bitmap(candidate);
  const BmpReaderError result = bitmap.parseHeaders();
  const bool ioError = candidate.getError() != 0;
  const bool closed = candidate.close();
  if (ioError || !closed || result == BmpReaderError::FileInvalid || result == BmpReaderError::SeekStartFailed ||
      result == BmpReaderError::SeekPixelDataFailed) {
    return BitmapFileStatus::IoError;
  }
  return result == BmpReaderError::Ok ? BitmapFileStatus::Valid : BitmapFileStatus::Invalid;
}

bool Bitmap::validateFile(const char* path, void*) { return inspectFile(path) == BitmapFileStatus::Valid; }

BitmapCacheState Bitmap::inspectDerivedCache(const std::string& finalPath) {
  const std::string backupPath = finalPath + ".bak";
  const BitmapFileStatus finalStatus = inspectFile(finalPath.c_str());
  if (finalStatus == BitmapFileStatus::Valid) {
    // The final was already verified above. Revalidating it through recover()
    // adds SD I/O and can replace it with a stale backup if that second read
    // fails transiently. Backup cleanup remains best-effort.
    if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
    return BitmapCacheState::Ready;
  }
  if (finalStatus == BitmapFileStatus::IoError) return BitmapCacheState::IoError;

  const BitmapFileStatus backupStatus = inspectFile(backupPath.c_str());
  if (backupStatus == BitmapFileStatus::IoError) return BitmapCacheState::IoError;
  if (backupStatus == BitmapFileStatus::Valid) {
    // Both candidates have already been classified. Avoid asking the generic
    // recovery helper to read the backup again: a transient second-read error
    // would otherwise discard this verified recovery candidate.
    if (finalStatus == BitmapFileStatus::Invalid && !Storage.remove(finalPath.c_str())) {
      return BitmapCacheState::IoError;
    }
    if (!Storage.rename(backupPath.c_str(), finalPath.c_str())) return BitmapCacheState::IoError;
    return inspectFile(finalPath.c_str()) == BitmapFileStatus::Valid ? BitmapCacheState::Ready
                                                                     : BitmapCacheState::IoError;
  }
  if (backupStatus == BitmapFileStatus::Invalid && !Storage.remove(backupPath.c_str())) {
    return BitmapCacheState::IoError;
  }
  return BitmapCacheState::Generate;
}

bool Bitmap::readLE16(HalFile& f, uint16_t& value) {
  uint8_t bytes[2];
  if (f.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  value = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
  return true;
}

bool Bitmap::readLE32(HalFile& f, uint32_t& value) {
  uint8_t bytes[4];
  if (f.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
          (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

const char* Bitmap::errorToString(BmpReaderError err) {
  switch (err) {
    case BmpReaderError::Ok:
      return "Ok";
    case BmpReaderError::FileInvalid:
      return "FileInvalid";
    case BmpReaderError::SeekStartFailed:
      return "SeekStartFailed";
    case BmpReaderError::ShortReadHeader:
      return "ShortReadHeader";
    case BmpReaderError::NotBMP:
      return "NotBMP (missing 'BM')";
    case BmpReaderError::DIBTooSmall:
      return "DIBTooSmall (<40 bytes)";
    case BmpReaderError::BadPlanes:
      return "BadPlanes (!= 1)";
    case BmpReaderError::UnsupportedBpp:
      return "UnsupportedBpp (expected 1, 2, 4, 8, 24, or 32)";
    case BmpReaderError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB or BI_BITFIELDS for 32bpp)";
    case BmpReaderError::BadDimensions:
      return "BadDimensions";
    case BmpReaderError::ImageTooLarge:
      return "ImageTooLarge (max 2048x3072)";
    case BmpReaderError::PaletteTooLarge:
      return "PaletteTooLarge";
    case BmpReaderError::PixelDataOutOfBounds:
      return "PixelDataOutOfBounds";

    case BmpReaderError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpReaderError::BufferTooSmall:
      return "BufferTooSmall";

    case BmpReaderError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpReaderError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpReaderError Bitmap::parseHeaders() {
  if (!file) return BmpReaderError::FileInvalid;
  if (!file.seek(0)) return BmpReaderError::SeekStartFailed;
  const uint64_t fileSize = file.fileSize64();

  // --- BMP FILE HEADER ---
  uint16_t bfType = 0;
  uint32_t declaredFileSize = 0;
  if (!readLE16(file, bfType) || !readLE32(file, declaredFileSize) || !file.seekCur(4) || !readLE32(file, bfOffBits)) {
    return BmpReaderError::ShortReadHeader;
  }
  if (bfType != 0x4D42) return BmpReaderError::NotBMP;

  // --- DIB HEADER ---
  uint32_t biSize = 0;
  uint32_t rawWidth = 0;
  uint32_t rawHeightBits = 0;
  uint16_t planes = 0;
  uint32_t comp = 0;
  if (!readLE32(file, biSize)) return BmpReaderError::ShortReadHeader;
  if (biSize < 40) return BmpReaderError::DIBTooSmall;
  if (!readLE32(file, rawWidth) || !readLE32(file, rawHeightBits) || !readLE16(file, planes) || !readLE16(file, bpp) ||
      !readLE32(file, comp)) {
    return BmpReaderError::ShortReadHeader;
  }
  width = static_cast<int32_t>(rawWidth);
  const auto rawHeight = static_cast<int32_t>(rawHeightBits);
  if (rawHeight == INT32_MIN) return BmpReaderError::BadDimensions;
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;
  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32;

  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!validBpp) return BmpReaderError::UnsupportedBpp;
  // Allow BI_RGB (0) for all, and BI_BITFIELDS (3) for 32bpp which is common for BGRA masks.
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return BmpReaderError::UnsupportedCompression;

  if (!file.seekCur(12) || !readLE32(file, colorsUsed) || !file.seekCur(4) ||
      (biSize > 40 && !file.seekCur(static_cast<int64_t>(biSize - 40)))) {
    return BmpReaderError::ShortReadHeader;
  }
  // BMP spec: colorsUsed==0 means default (2^bpp for paletted formats)
  if (colorsUsed == 0 && bpp <= 8) colorsUsed = 1u << bpp;
  if (colorsUsed > 256u) return BmpReaderError::PaletteTooLarge;
  if (width <= 0 || height <= 0) return BmpReaderError::BadDimensions;

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    return BmpReaderError::ImageTooLarge;
  }

  const uint64_t rowBits = static_cast<uint64_t>(width) * bpp;
  const uint64_t calculatedRowBytes = ((rowBits + 31U) / 32U) * 4U;
  if (calculatedRowBytes == 0 || calculatedRowBytes > static_cast<uint64_t>(INT32_MAX)) {
    return BmpReaderError::ImageTooLarge;
  }
  rowBytes = static_cast<int>(calculatedRowBytes);

  const uint64_t paletteBytes = static_cast<uint64_t>(colorsUsed) * 4U;
  const uint64_t paletteStart = 14U + static_cast<uint64_t>(biSize);
  if (paletteStart > fileSize || paletteBytes > fileSize - paletteStart || bfOffBits < paletteStart + paletteBytes ||
      bfOffBits > fileSize) {
    return BmpReaderError::PixelDataOutOfBounds;
  }
  const uint64_t pixelBytes = calculatedRowBytes * static_cast<uint64_t>(height);
  if (pixelBytes > fileSize - bfOffBits ||
      (declaredFileSize != 0 &&
       (declaredFileSize < bfOffBits || declaredFileSize > fileSize || pixelBytes > declaredFileSize - bfOffBits))) {
    return BmpReaderError::PixelDataOutOfBounds;
  }

  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    for (uint32_t i = 0; i < colorsUsed; i++) {
      uint8_t rgb[4];
      if (file.read(rgb, sizeof(rgb)) != static_cast<int>(sizeof(rgb))) return BmpReaderError::ShortReadHeader;
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Check if palette luminances map cleanly to the display's 4 native gray levels.
  // Native levels are 0, 85, 170, 255 — i.e. values where (lum >> 6) is lossless.
  // If all palette entries are near a native level, we can skip dithering entirely.
  nativePalette = bpp <= 2;  // 1-bit and 2-bit are always native
  if (!nativePalette && colorsUsed > 0) {
    nativePalette = true;
    for (uint32_t i = 0; i < colorsUsed; i++) {
      const uint8_t lum = paletteLum[i];
      const uint8_t level = lum >> 6;            // quantize to 0-3
      const uint8_t reconstructed = level * 85;  // back to 0, 85, 170, 255
      if (lum > reconstructed + 21 || lum + 21 < reconstructed) {
        nativePalette = false;  // luminance is too far from any native level
        break;
      }
    }
  }

  // Decide pixel processing strategy:
  //  - Native palette → direct mapping, no processing needed
  //  - High-color + dithering enabled → error-diffusion dithering (Atkinson or Floyd-Steinberg)
  //  - High-color + dithering disabled → simple quantization (no error diffusion)
  const bool highColor = !nativePalette;
  if (highColor && dithering) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(width, std::nothrow);
      if (!atkinsonDitherer || !atkinsonDitherer->valid()) {
        delete atkinsonDitherer;
        atkinsonDitherer = nullptr;
        return BmpReaderError::OomRowBuffer;
      }
    } else {
      fsDitherer = new (std::nothrow) FloydSteinbergDitherer(width, std::nothrow);
      if (!fsDitherer || !fsDitherer->valid()) {
        delete fsDitherer;
        fsDitherer = nullptr;
        return BmpReaderError::OomRowBuffer;
      }
    }
  }

  return BmpReaderError::Ok;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readNextRow(uint8_t* data, uint8_t* rowBuffer) const {
  // Note: rowBuffer should be pre-allocated by the caller to size 'rowBytes'
  if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;

  prevRowY += 1;

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  // Helper lambda to pack 2bpp color into the output stream
  auto packPixel = [&](const uint8_t lum) {
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(adjustPixel(lum), currentX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(adjustPixel(lum), currentX);
    } else {
      if (nativePalette) {
        // Palette matches native gray levels: direct mapping (still apply brightness/contrast/gamma)
        color = static_cast<uint8_t>(adjustPixel(lum) >> 6);
      } else {
        // Non-native palette with dithering disabled: simple quantization
        color = quantize(adjustPixel(lum), currentX, prevRowY);
      }
    }
    currentOutByte |= (color << bitShift);
    if (bitShift == 0) {
      *outPtr++ = currentOutByte;
      currentOutByte = 0;
      bitShift = 6;
    } else {
      bitShift -= 2;
    }
    currentX++;
  };

  uint8_t lum;

  switch (bpp) {
    case 32: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 4;
      }
      break;
    }
    case 24: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 3;
      }
      break;
    }
    case 8: {
      for (int x = 0; x < width; x++) {
        packPixel(paletteLum[rowBuffer[x]]);
      }
      break;
    }
    case 4: {
      for (int x = 0; x < width; x++) {
        const uint8_t nibble = (x & 1) ? (rowBuffer[x >> 1] & 0x0F) : (rowBuffer[x >> 1] >> 4);
        packPixel(paletteLum[nibble]);
      }
      break;
    }
    case 2: {
      for (int x = 0; x < width; x++) {
        lum = paletteLum[(rowBuffer[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03];
        packPixel(lum);
      }
      break;
    }
    case 1: {
      for (int x = 0; x < width; x++) {
        // Get palette index (0 or 1) from bit at position x
        const uint8_t palIndex = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
        // Use palette lookup for proper black/white mapping
        lum = paletteLum[palIndex];
        packPixel(lum);
      }
      break;
    }
    default:
      return BmpReaderError::UnsupportedBpp;
  }

  if (atkinsonDitherer)
    atkinsonDitherer->nextRow();
  else if (fsDitherer)
    fsDitherer->nextRow();

  // Flush remaining bits if width is not a multiple of 4
  if (bitShift != 6) *outPtr = currentOutByte;

  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::readPackedRows(uint8_t* data, const size_t capacity) const {
  if (bpp != 1 || !data) return BmpReaderError::BufferTooSmall;
  const uint64_t bytes = static_cast<uint64_t>(rowBytes) * static_cast<uint64_t>(height);
  if (bytes == 0 || bytes > capacity || bytes > static_cast<uint64_t>(INT_MAX)) {
    return BmpReaderError::BufferTooSmall;
  }
  return file.read(data, static_cast<size_t>(bytes)) == static_cast<int>(bytes) ? BmpReaderError::Ok
                                                                                : BmpReaderError::ShortReadRow;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Reset dithering when rewinding
  if (fsDitherer) fsDitherer->reset();
  if (atkinsonDitherer) atkinsonDitherer->reset();

  return BmpReaderError::Ok;
}
