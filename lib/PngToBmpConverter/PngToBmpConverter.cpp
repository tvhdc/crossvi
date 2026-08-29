#include "PngToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <new>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_err.h>
#include <esp_task_wdt.h>
#endif

#include "BitmapHelpers.h"
#include "PngImageSafety.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Same as JpegToBmpConverter for consistency
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr bool USE_PRESCALE = true;
// ============================================================================

namespace {

class CheckedPrint final : public Print {
 public:
  explicit CheckedPrint(Print& output) : output_(output) {}
  using Print::write;

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, const size_t length) override {
    if (failed_) return 0;
    const size_t written = output_.write(data, length);
    if (written != length) failed_ = true;
    return written;
  }

  bool failed() const { return failed_; }

 private:
  Print& output_;
  bool failed_ = false;
};

}  // namespace

// BMP writing helpers (same as JpegToBmpConverter)
inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Paeth predictor function per PNG spec
inline uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  int p = static_cast<int>(a) + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

namespace {
// PNG constants
uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// PNG color types
enum PngColorType : uint8_t {
  PNG_COLOR_GRAYSCALE = 0,
  PNG_COLOR_RGB = 2,
  PNG_COLOR_PALETTE = 3,
  PNG_COLOR_GRAYSCALE_ALPHA = 4,
  PNG_COLOR_RGBA = 6,
};

// PNG filter types
enum PngFilter : uint8_t {
  PNG_FILTER_NONE = 0,
  PNG_FILTER_SUB = 1,
  PNG_FILTER_UP = 2,
  PNG_FILTER_AVERAGE = 3,
  PNG_FILTER_PAETH = 4,
};

// Read a big-endian 32-bit value from file
bool readBE32(HalFile& file, uint32_t& value) {
  uint8_t buf[4];
  if (file.read(buf, 4) != 4) return false;
  value = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
          (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
  return true;
}

void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 3) / 4 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 14 + 40 + paletteSize);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 8);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 256);
  write32(bmpOut, 256);

  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(0));
  }
}

void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 62);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 1);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 2);
  write32(bmpOut, 2);

  uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 2);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);

  uint8_t palette[16] = {0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x00,
                         0xAA, 0xAA, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}
}  // namespace

// Context for streaming PNG decompression
struct PngDecodeContext {
  InflateStream reader;
  HalFile* file;

  // PNG image properties
  uint32_t width;
  uint32_t height;
  uint8_t bitDepth;
  uint8_t colorType;
  uint8_t bytesPerPixel;  // after expanding sub-byte depths
  uint32_t rawRowBytes;   // bytes per raw row (without filter byte)

  // Scanline buffers
  uint8_t* currentRow;   // current defiltered scanline
  uint8_t* previousRow;  // previous defiltered scanline

  // Chunk reading state
  uint32_t chunkBytesRemaining;  // bytes left in current IDAT chunk
  bool idatFinished;             // no more IDAT chunks

  // File read buffer for feeding the inflate stream
  uint8_t readBuf[2048];

  // Palette for indexed color (type 3)
  uint8_t palette[256 * 3];
  int paletteSize;
};

// Read the next IDAT chunk header, skipping non-IDAT chunks
// Returns true if an IDAT chunk was found
static bool findNextIdatChunk(PngDecodeContext& ctx) {
  while (true) {
    uint32_t chunkLen;
    if (!readBE32(*ctx.file, chunkLen)) return false;

    uint8_t chunkType[4];
    if (ctx.file->read(chunkType, 4) != 4) return false;

    if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      return true;
    }

    // Skip this chunk's data + 4-byte CRC
    // Use seek to skip efficiently
    if (!ctx.file->seekCur(static_cast<int64_t>(chunkLen) + 4)) return false;

    // If we hit IEND, there are no more chunks
    if (memcmp(chunkType, "IEND", 4) == 0) {
      return false;
    }
  }
}

// Fill callback: reads the next batch of IDAT data from the file
static size_t pngIdatFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<PngDecodeContext*>(vctx);

  if (ctx->idatFinished) return 0;

  // Skip 4-byte CRC and find next IDAT chunk when current chunk is exhausted
  while (ctx->chunkBytesRemaining == 0) {
    if (!ctx->file->seekCur(4)) {  // skip 4-byte CRC of previous IDAT
      ctx->idatFinished = true;
      return 0;
    }
    if (!findNextIdatChunk(*ctx)) {
      ctx->idatFinished = true;
      return 0;
    }
  }

  // Read from current IDAT chunk into the read buffer
  size_t toRead = sizeof(ctx->readBuf);
  if (toRead > ctx->chunkBytesRemaining) toRead = ctx->chunkBytesRemaining;

  const int bytesRead = ctx->file->read(ctx->readBuf, toRead);
  if (bytesRead <= 0) {
    ctx->idatFinished = true;
    return 0;
  }

  ctx->chunkBytesRemaining -= bytesRead;
  *data = ctx->readBuf;
  return static_cast<size_t>(bytesRead);
}

// Decode one scanline: decompress filter byte + raw bytes, then unfilter
static bool decodeScanline(PngDecodeContext& ctx) {
  // Decompress filter byte
  uint8_t filterType;
  if (!ctx.reader.read(&filterType, 1)) return false;

  // Decompress raw row data into currentRow
  if (!ctx.reader.read(ctx.currentRow, ctx.rawRowBytes)) return false;

  // Apply reverse filter
  const int bpp = ctx.bytesPerPixel;

  switch (filterType) {
    case PNG_FILTER_NONE:
      break;

    case PNG_FILTER_SUB:
      for (uint32_t i = bpp; i < ctx.rawRowBytes; i++) {
        ctx.currentRow[i] += ctx.currentRow[i - bpp];
      }
      break;

    case PNG_FILTER_UP:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        ctx.currentRow[i] += ctx.previousRow[i];
      }
      break;

    case PNG_FILTER_AVERAGE:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        ctx.currentRow[i] += (a + b) / 2;
      }
      break;

    case PNG_FILTER_PAETH:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        uint8_t c = (i >= static_cast<uint32_t>(bpp)) ? ctx.previousRow[i - bpp] : 0;
        ctx.currentRow[i] += paethPredictor(a, b, c);
      }
      break;

    default:
      LOG_ERR("PNG", "Unknown filter type: %d", filterType);
      return false;
  }

  return true;
}

// Batch-convert an entire scanline to grayscale.
// Branches once on colorType/bitDepth, then runs a tight loop for the whole row.
static void convertScanlineToGray(const PngDecodeContext& ctx, uint8_t* grayRow) {
  const uint8_t* src = ctx.currentRow;
  const uint32_t w = ctx.width;

  switch (ctx.colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (ctx.bitDepth == 8) {
        memcpy(grayRow, src, w);
      } else if (ctx.bitDepth == 16) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        const int ppb = 8 / ctx.bitDepth;
        const uint8_t mask = (1 << ctx.bitDepth) - 1;
        for (uint32_t x = 0; x < w; x++) {
          int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
          grayRow[x] = (src[x / ppb] >> shift & mask) * 255 / mask;
        }
      }
      break;

    case PNG_COLOR_RGB:
      if (ctx.bitDepth == 8) {
        // Fast path: most common EPUB cover format
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 3;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 6] * 25 + src[x * 6 + 2] * 50 + src[x * 6 + 4] * 25) / 100;
        }
      }
      break;

    case PNG_COLOR_PALETTE: {
      const int ppb = 8 / ctx.bitDepth;
      const uint8_t mask = (1 << ctx.bitDepth) - 1;
      const uint8_t* pal = ctx.palette;
      const int palSize = ctx.paletteSize;
      for (uint32_t x = 0; x < w; x++) {
        int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
        uint8_t idx = (src[x / ppb] >> shift) & mask;
        if (idx >= palSize) idx = 0;
        grayRow[x] = (pal[idx * 3] * 25 + pal[idx * 3 + 1] * 50 + pal[idx * 3 + 2] * 25) / 100;
      }
      break;
    }

    case PNG_COLOR_GRAYSCALE_ALPHA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 4];
      }
      break;

    case PNG_COLOR_RGBA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 4;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 8] * 25 + src[x * 8 + 2] * 50 + src[x * 8 + 4] * 25) / 100;
        }
      }
      break;

    default:
      memset(grayRow, 128, w);
      break;
  }
}

namespace {

struct PngTransparency {
  uint8_t paletteAlpha[256];
  bool hasKey;
  uint16_t gray;
  uint16_t red;
  uint16_t green;
  uint16_t blue;
};

uint16_t readBe16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
}

void putLe16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void putLe32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

bool writeBgraBmpHeader(Print& output, const uint32_t width, const uint32_t height) {
  constexpr uint32_t HEADER_SIZE = 70;
  const uint32_t imageSize = width * height * 4U;
  uint8_t header[HEADER_SIZE] = {};
  header[0] = 'B';
  header[1] = 'M';
  putLe32(header + 2, HEADER_SIZE + imageSize);
  putLe32(header + 10, HEADER_SIZE);
  putLe32(header + 14, 40);
  putLe32(header + 18, width);
  putLe32(header + 22, 0U - height);  // Negative height: rows are stored top-down.
  putLe16(header + 26, 1);
  putLe16(header + 28, 32);
  putLe32(header + 30, 3);  // BI_BITFIELDS
  putLe32(header + 34, imageSize);
  putLe32(header + 38, 2835);
  putLe32(header + 42, 2835);
  putLe32(header + 54, 0x00FF0000U);
  putLe32(header + 58, 0x0000FF00U);
  putLe32(header + 62, 0x000000FFU);
  putLe32(header + 66, 0xFF000000U);
  return output.write(header, sizeof(header)) == sizeof(header);
}

bool calculateBgraRowLayout(const uint32_t width, const uint8_t colorType, const uint8_t bitDepth,
                            uint8_t& bytesPerPixel, uint32_t& rawRowBytes) {
  switch (colorType) {
    case PNG_COLOR_GRAYSCALE:
      bytesPerPixel = bitDepth == 16 ? 2 : 1;
      rawRowBytes = bitDepth < 8 ? (width * bitDepth + 7U) / 8U : width * bytesPerPixel;
      break;
    case PNG_COLOR_RGB:
      bytesPerPixel = bitDepth == 16 ? 6 : 3;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_PALETTE:
      bytesPerPixel = 1;
      rawRowBytes = (width * bitDepth + 7U) / 8U;
      break;
    case PNG_COLOR_GRAYSCALE_ALPHA:
      bytesPerPixel = bitDepth == 16 ? 4 : 2;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_RGBA:
      bytesPerPixel = bitDepth == 16 ? 8 : 4;
      rawRowBytes = width * bytesPerPixel;
      break;
    default:
      return false;
  }
  return rawRowBytes <= 16384U;
}

bool readTransparencyChunk(HalFile& input, const PngDecodeContext& ctx, PngTransparency& transparency,
                           const uint32_t chunkLength) {
  uint8_t key[6];
  switch (ctx.colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (chunkLength != 2 || input.read(key, 2) != 2) return false;
      transparency.hasKey = true;
      transparency.gray = readBe16(key);
      return true;
    case PNG_COLOR_RGB:
      if (chunkLength != 6 || input.read(key, 6) != 6) return false;
      transparency.hasKey = true;
      transparency.red = readBe16(key);
      transparency.green = readBe16(key + 2);
      transparency.blue = readBe16(key + 4);
      return true;
    case PNG_COLOR_PALETTE:
      if (ctx.paletteSize == 0 || chunkLength == 0 || chunkLength > static_cast<uint32_t>(ctx.paletteSize) ||
          input.read(transparency.paletteAlpha, chunkLength) != static_cast<int>(chunkLength)) {
        return false;
      }
      return true;
    default:
      return false;
  }
}

bool prepareBgraDecoder(HalFile& input, PngDecodeContext& ctx, PngTransparency& transparency) {
  uint8_t signature[8];
  if (input.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      memcmp(signature, PNG_SIGNATURE, sizeof(signature)) != 0) {
    return false;
  }

  uint32_t ihdrLength;
  uint8_t ihdrType[4];
  if (!readBE32(input, ihdrLength) || ihdrLength != 13 || input.read(ihdrType, sizeof(ihdrType)) != 4 ||
      memcmp(ihdrType, "IHDR", sizeof(ihdrType)) != 0 || !readBE32(input, ctx.width) || !readBE32(input, ctx.height)) {
    return false;
  }

  uint8_t ihdrRest[5];
  if (input.read(ihdrRest, sizeof(ihdrRest)) != static_cast<int>(sizeof(ihdrRest)) || !input.seekCur(4)) return false;
  ctx.bitDepth = ihdrRest[0];
  ctx.colorType = ihdrRest[1];
  if (!png_image_safety::validIhdr(ihdrLength, ctx.width, ctx.height, ctx.bitDepth, ctx.colorType, ihdrRest[2],
                                   ihdrRest[3], ihdrRest[4]) ||
      !calculateBgraRowLayout(ctx.width, ctx.colorType, ctx.bitDepth, ctx.bytesPerPixel, ctx.rawRowBytes)) {
    return false;
  }

  ctx.file = &input;
  memset(transparency.paletteAlpha, 0xFF, sizeof(transparency.paletteAlpha));
  bool foundIdat = false;
  bool foundPalette = false;
  bool foundTransparency = false;
  while (!foundIdat) {
    uint32_t chunkLength;
    uint8_t chunkType[4];
    if (!readBE32(input, chunkLength) || input.read(chunkType, sizeof(chunkType)) != 4) return false;

    if (memcmp(chunkType, "PLTE", sizeof(chunkType)) == 0) {
      if (foundPalette || chunkLength == 0 || chunkLength > sizeof(ctx.palette) || chunkLength % 3U != 0) return false;
      ctx.paletteSize = static_cast<int>(chunkLength / 3U);
      if (input.read(ctx.palette, chunkLength) != static_cast<int>(chunkLength) || !input.seekCur(4)) return false;
      foundPalette = true;
    } else if (memcmp(chunkType, "tRNS", sizeof(chunkType)) == 0) {
      if (foundTransparency || !readTransparencyChunk(input, ctx, transparency, chunkLength) || !input.seekCur(4)) {
        return false;
      }
      foundTransparency = true;
    } else if (memcmp(chunkType, "IDAT", sizeof(chunkType)) == 0) {
      ctx.chunkBytesRemaining = chunkLength;
      foundIdat = true;
    } else if (memcmp(chunkType, "IEND", sizeof(chunkType)) == 0) {
      return false;
    } else if (!input.seekCur(static_cast<int64_t>(chunkLength) + 4)) {
      return false;
    }
  }

  if (ctx.colorType == PNG_COLOR_PALETTE &&
      (!foundPalette || ctx.paletteSize > static_cast<int>(uint16_t{1} << ctx.bitDepth))) {
    return false;
  }
  return true;
}

bool calculateFitDimensions(const uint32_t width, const uint32_t height, const int targetMaxWidth,
                            const int targetMaxHeight, uint32_t& outputWidth, uint32_t& outputHeight) {
  if (targetMaxWidth <= 0 || targetMaxHeight <= 0) return false;
  outputWidth = width;
  outputHeight = height;
  if (width <= static_cast<uint32_t>(targetMaxWidth) && height <= static_cast<uint32_t>(targetMaxHeight)) return true;

  const uint64_t targetWidth = static_cast<uint32_t>(targetMaxWidth);
  const uint64_t targetHeight = static_cast<uint32_t>(targetMaxHeight);
  if (static_cast<uint64_t>(width) * targetHeight > static_cast<uint64_t>(height) * targetWidth) {
    outputWidth = static_cast<uint32_t>(targetWidth);
    outputHeight = static_cast<uint32_t>(static_cast<uint64_t>(height) * targetWidth / width);
  } else {
    outputHeight = static_cast<uint32_t>(targetHeight);
    outputWidth = static_cast<uint32_t>(static_cast<uint64_t>(width) * targetHeight / height);
  }
  if (outputWidth == 0) outputWidth = 1;
  if (outputHeight == 0) outputHeight = 1;
  return true;
}

bool pixelToBgra(const PngDecodeContext& ctx, const PngTransparency& transparency, const uint32_t x, uint8_t* output) {
  const uint8_t* row = ctx.currentRow;
  uint16_t red = 0;
  uint16_t green = 0;
  uint16_t blue = 0;
  uint16_t alpha = 255;

  switch (ctx.colorType) {
    case PNG_COLOR_GRAYSCALE: {
      uint16_t sample;
      uint8_t gray;
      if (ctx.bitDepth == 16) {
        sample = readBe16(row + x * 2U);
        gray = row[x * 2U];
      } else if (ctx.bitDepth == 8) {
        sample = row[x];
        gray = row[x];
      } else {
        const uint8_t samplesPerByte = 8U / ctx.bitDepth;
        const uint8_t mask = static_cast<uint8_t>((1U << ctx.bitDepth) - 1U);
        const uint8_t shift = static_cast<uint8_t>((samplesPerByte - 1U - x % samplesPerByte) * ctx.bitDepth);
        sample = static_cast<uint16_t>((row[x / samplesPerByte] >> shift) & mask);
        gray = static_cast<uint8_t>(sample * 255U / mask);
      }
      red = green = blue = gray;
      if (transparency.hasKey && sample == transparency.gray) alpha = 0;
      break;
    }
    case PNG_COLOR_RGB:
      if (ctx.bitDepth == 16) {
        const uint8_t* pixel = row + x * 6U;
        const uint16_t redSample = readBe16(pixel);
        const uint16_t greenSample = readBe16(pixel + 2);
        const uint16_t blueSample = readBe16(pixel + 4);
        red = pixel[0];
        green = pixel[2];
        blue = pixel[4];
        if (transparency.hasKey && redSample == transparency.red && greenSample == transparency.green &&
            blueSample == transparency.blue) {
          alpha = 0;
        }
      } else {
        const uint8_t* pixel = row + x * 3U;
        red = pixel[0];
        green = pixel[1];
        blue = pixel[2];
        if (transparency.hasKey && red == transparency.red && green == transparency.green &&
            blue == transparency.blue) {
          alpha = 0;
        }
      }
      break;
    case PNG_COLOR_PALETTE: {
      const uint8_t samplesPerByte = 8U / ctx.bitDepth;
      const uint8_t mask = static_cast<uint8_t>((1U << ctx.bitDepth) - 1U);
      const uint8_t shift = static_cast<uint8_t>((samplesPerByte - 1U - x % samplesPerByte) * ctx.bitDepth);
      const uint8_t index = static_cast<uint8_t>((row[x / samplesPerByte] >> shift) & mask);
      if (index >= ctx.paletteSize) return false;
      red = ctx.palette[index * 3U];
      green = ctx.palette[index * 3U + 1U];
      blue = ctx.palette[index * 3U + 2U];
      alpha = transparency.paletteAlpha[index];
      break;
    }
    case PNG_COLOR_GRAYSCALE_ALPHA:
      if (ctx.bitDepth == 16) {
        const uint8_t* pixel = row + x * 4U;
        red = green = blue = pixel[0];
        alpha = pixel[2];
      } else {
        const uint8_t* pixel = row + x * 2U;
        red = green = blue = pixel[0];
        alpha = pixel[1];
      }
      break;
    case PNG_COLOR_RGBA:
      if (ctx.bitDepth == 16) {
        const uint8_t* pixel = row + x * 8U;
        red = pixel[0];
        green = pixel[2];
        blue = pixel[4];
        alpha = pixel[6];
      } else {
        const uint8_t* pixel = row + x * 4U;
        red = pixel[0];
        green = pixel[1];
        blue = pixel[2];
        alpha = pixel[3];
      }
      break;
    default:
      return false;
  }

  output[0] = static_cast<uint8_t>(blue);
  output[1] = static_cast<uint8_t>(green);
  output[2] = static_cast<uint8_t>(red);
  output[3] = static_cast<uint8_t>(alpha);
  return true;
}

bool finishDecodedPng(PngDecodeContext& ctx) {
  uint8_t extraByte;
  size_t produced = 0;
  if (ctx.reader.readAtMost(&extraByte, 1, &produced) != InflateStream::Status::Done || produced != 0 ||
      ctx.chunkBytesRemaining != 0) {
    return false;
  }

  const auto canSkip = [&ctx](const uint64_t bytes) {
    const uint64_t position = ctx.file->position();
    const uint64_t size = ctx.file->fileSize64();
    return position <= size && bytes <= size - position;
  };
  if (!canSkip(4) || !ctx.file->seekCur(4)) return false;  // Last IDAT CRC.

  while (true) {
    uint32_t chunkLength;
    uint8_t chunkType[4];
    if (!readBE32(*ctx.file, chunkLength) || ctx.file->read(chunkType, sizeof(chunkType)) != 4) return false;
    if (memcmp(chunkType, "IEND", sizeof(chunkType)) == 0) {
      return chunkLength == 0 && canSkip(4) && ctx.file->seekCur(4);
    }
    if (memcmp(chunkType, "IDAT", sizeof(chunkType)) == 0 && chunkLength != 0) return false;
    if (!canSkip(static_cast<uint64_t>(chunkLength) + 4U) ||
        !ctx.file->seekCur(static_cast<int64_t>(chunkLength) + 4)) {
      return false;
    }
  }
}

void serviceLongPngConversion() {
#if defined(ARDUINO_ARCH_ESP32)
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  yield();
#endif
}

}  // namespace

bool PngToBmpConverter::pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop) {
  LOG_DBG("PNG", "Converting PNG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // Verify PNG signature
  uint8_t sig[8];
  if (pngFile.read(sig, 8) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) {
    LOG_ERR("PNG", "Invalid PNG signature");
    return false;
  }

  // Read IHDR chunk
  uint32_t ihdrLen;
  if (!readBE32(pngFile, ihdrLen)) return false;
  if (ihdrLen != 13) {
    LOG_ERR("PNG", "Invalid IHDR length: %u", ihdrLen);
    return false;
  }

  uint8_t ihdrType[4];
  if (pngFile.read(ihdrType, 4) != 4 || memcmp(ihdrType, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Missing IHDR chunk");
    return false;
  }

  uint32_t width, height;
  if (!readBE32(pngFile, width) || !readBE32(pngFile, height)) return false;

  uint8_t ihdrRest[5];
  if (pngFile.read(ihdrRest, 5) != 5) return false;

  uint8_t bitDepth = ihdrRest[0];
  uint8_t colorType = ihdrRest[1];
  uint8_t compression = ihdrRest[2];
  uint8_t filter = ihdrRest[3];
  uint8_t interlace = ihdrRest[4];

  // Skip IHDR CRC
  if (!pngFile.seekCur(4)) return false;

  LOG_DBG("PNG", "Image: %ux%u, depth=%u, color=%u, interlace=%u", width, height, bitDepth, colorType, interlace);

  if (!png_image_safety::validIhdr(ihdrLen, width, height, bitDepth, colorType, compression, filter, interlace)) {
    LOG_ERR("PNG", "Invalid or unsupported IHDR");
    return false;
  }

  // Calculate bytes per pixel and raw row bytes
  uint8_t bytesPerPixel;
  uint32_t rawRowBytes;

  switch (colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (bitDepth == 16) {
        bytesPerPixel = 2;
        rawRowBytes = width * 2;
      } else if (bitDepth == 8) {
        bytesPerPixel = 1;
        rawRowBytes = width;
      } else {
        // Sub-byte: 1, 2, or 4 bits
        bytesPerPixel = 1;
        rawRowBytes = (width * bitDepth + 7) / 8;
      }
      break;
    case PNG_COLOR_RGB:
      bytesPerPixel = (bitDepth == 16) ? 6 : 3;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_PALETTE:
      bytesPerPixel = 1;
      rawRowBytes = (width * bitDepth + 7) / 8;
      break;
    case PNG_COLOR_GRAYSCALE_ALPHA:
      bytesPerPixel = (bitDepth == 16) ? 4 : 2;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_RGBA:
      bytesPerPixel = (bitDepth == 16) ? 8 : 4;
      rawRowBytes = width * bytesPerPixel;
      break;
    default:
      LOG_ERR("PNG", "Unsupported color type: %d", colorType);
      return false;
  }

  // Validate raw row bytes won't cause memory issues
  if (rawRowBytes > 16384) {
    LOG_ERR("PNG", "Row too large: %u bytes", rawRowBytes);
    return false;
  }

  int outWidth = 0;
  int outHeight = 0;
  if (!png_image_safety::calculateOutputDimensions(width, height, targetWidth, targetHeight, crop, outWidth,
                                                   outHeight)) {
    LOG_ERR("PNG", "Output dimensions exceed safety limits");
    return false;
  }
  const bool needsScaling = static_cast<uint32_t>(outWidth) != width || static_cast<uint32_t>(outHeight) != height;
  const uint32_t scaleX_fp = needsScaling ? (width << 16) / static_cast<uint32_t>(outWidth) : 65536;
  const uint32_t scaleY_fp = needsScaling ? (height << 16) / static_cast<uint32_t>(outHeight) : 65536;
  if (needsScaling) {
    LOG_DBG("PNG", "Scaling %ux%u -> %dx%d (target %dx%d)", width, height, outWidth, outHeight, targetWidth,
            targetHeight);
  }

  // Initialize decode context
  PngDecodeContext ctx = {};
  ctx.file = &pngFile;
  ctx.width = width;
  ctx.height = height;
  ctx.bitDepth = bitDepth;
  ctx.colorType = colorType;
  ctx.bytesPerPixel = bytesPerPixel;
  ctx.rawRowBytes = rawRowBytes;
  ctx.paletteSize = 0;

  // Allocate scanline buffers
  ctx.currentRow = static_cast<uint8_t*>(malloc(rawRowBytes));
  ctx.previousRow = static_cast<uint8_t*>(calloc(rawRowBytes, 1));
  if (!ctx.currentRow || !ctx.previousRow) {
    LOG_ERR("PNG", "Failed to allocate scanline buffers (%u bytes each)", rawRowBytes);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Scan for PLTE chunk (palette) and first IDAT chunk
  // We need to read chunks until we find IDAT, collecting PLTE along the way
  bool foundIdat = false;
  while (!foundIdat) {
    uint32_t chunkLen;
    if (!readBE32(pngFile, chunkLen)) break;

    uint8_t chunkType[4];
    if (pngFile.read(chunkType, 4) != 4) break;

    if (memcmp(chunkType, "PLTE", 4) == 0) {
      int entries = chunkLen / 3;
      if (entries > 256) entries = 256;
      ctx.paletteSize = entries;
      size_t palBytes = entries * 3;
      if (pngFile.read(ctx.palette, palBytes) != static_cast<int>(palBytes)) break;
      // Skip any remaining palette data
      if (chunkLen > palBytes && !pngFile.seekCur(static_cast<int64_t>(chunkLen - palBytes))) break;
      if (!pngFile.seekCur(4)) break;  // CRC
    } else if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      foundIdat = true;
    } else if (memcmp(chunkType, "IEND", 4) == 0) {
      break;
    } else {
      // Skip unknown chunk
      if (!pngFile.seekCur(static_cast<int64_t>(chunkLen) + 4)) break;
    }
  }

  if (!foundIdat) {
    LOG_ERR("PNG", "No IDAT chunk found");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }
  if (colorType == PNG_COLOR_PALETTE && ctx.paletteSize == 0) {
    LOG_ERR("PNG", "Palette PNG is missing PLTE data");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Initialize streaming decompressor with 32KB window for back-reference history
  if (!ctx.reader.init(true)) {
    LOG_ERR("PNG", "Failed to init inflate stream");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }
  ctx.reader.setFill(pngIdatFillCallback, &ctx);
  // PNG IDAT data is zlib-wrapped (2-byte header + trailing adler32)
  ctx.reader.setZlibWrapped();

  // Calculate BMP row size. The header is written only after every working
  // allocation succeeds, so an OOM cannot leave a plausible partial BMP.
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  // Allocate BMP row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR("PNG", "Failed to allocate row buffer");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Create ditherers (same as JpegToBmpConverter)
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;

  if (oneBit) {
    atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth, std::nothrow);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth, std::nothrow);
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = new (std::nothrow) FloydSteinbergDitherer(outWidth, std::nothrow);
    }
  }

  const bool dithererValid = oneBit                             ? atkinson1BitDitherer && atkinson1BitDitherer->valid()
                             : !USE_8BIT_OUTPUT && USE_ATKINSON ? atkinsonDitherer && atkinsonDitherer->valid()
                             : !USE_8BIT_OUTPUT && USE_FLOYD_STEINBERG ? fsDitherer && fsDitherer->valid()
                                                                       : true;
  if (!dithererValid) {
    LOG_ERR("PNG", "Failed to allocate ditherer");
    delete atkinsonDitherer;
    delete fsDitherer;
    delete atkinson1BitDitherer;
    free(rowBuffer);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Scaling accumulators
  uint32_t* rowAccum = nullptr;
  uint16_t* rowCount = nullptr;
  int currentOutY = 0;
  uint32_t nextOutY_srcStart = 0;

  if (needsScaling) {
    rowAccum = new (std::nothrow) uint32_t[outWidth]();
    rowCount = new (std::nothrow) uint16_t[outWidth]();
    if (!rowAccum || !rowCount) {
      LOG_ERR("PNG", "Failed to allocate scaling buffers");
      delete[] rowAccum;
      delete[] rowCount;
      delete atkinsonDitherer;
      delete fsDitherer;
      delete atkinson1BitDitherer;
      free(rowBuffer);
      free(ctx.currentRow);
      free(ctx.previousRow);
      return false;
    }
    nextOutY_srcStart = scaleY_fp;
  }

  // Allocate grayscale row buffer - batch-convert each scanline to avoid
  // per-pixel getPixelGray() switch overhead in the hot loops
  auto* grayRow = static_cast<uint8_t*>(malloc(width));
  if (!grayRow) {
    LOG_ERR("PNG", "Failed to allocate grayscale row buffer");
    delete[] rowAccum;
    delete[] rowCount;
    delete atkinsonDitherer;
    delete fsDitherer;
    delete atkinson1BitDitherer;
    free(rowBuffer);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  CheckedPrint checkedOutput(bmpOut);
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(checkedOutput, outWidth, outHeight);
  } else if (oneBit) {
    writeBmpHeader1bit(checkedOutput, outWidth, outHeight);
  } else {
    writeBmpHeader2bit(checkedOutput, outWidth, outHeight);
  }

  bool success = !checkedOutput.failed();

  // Process each scanline
  for (uint32_t y = 0; success && y < height; y++) {
    // Decode one scanline
    if (!decodeScanline(ctx)) {
      LOG_ERR("PNG", "Failed to decode scanline %u", y);
      success = false;
      break;
    }

    // Batch-convert entire scanline to grayscale (one branch, tight loop)
    convertScanlineToGray(ctx, grayRow);

    if (!needsScaling) {
      // Direct output (no scaling)
      memset(rowBuffer, 0, bytesPerRow);

      if (USE_8BIT_OUTPUT && !oneBit) {
        for (int x = 0; x < outWidth; x++) {
          rowBuffer[x] = adjustPixel(grayRow[x]);
        }
      } else if (oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t bit =
              atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(grayRow[x], x) : quantize1bit(grayRow[x], x, y);
          const int byteIndex = x / 8;
          const int bitOffset = 7 - (x % 8);
          rowBuffer[byteIndex] |= (bit << bitOffset);
        }
        if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
      } else {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t gray = adjustPixel(grayRow[x]);
          uint8_t twoBit;
          if (atkinsonDitherer) {
            twoBit = atkinsonDitherer->processPixel(gray, x);
          } else if (fsDitherer) {
            twoBit = fsDitherer->processPixel(gray, x);
          } else {
            twoBit = quantize(gray, x, y);
          }
          const int byteIndex = (x * 2) / 8;
          const int bitOffset = 6 - ((x * 2) % 8);
          rowBuffer[byteIndex] |= (twoBit << bitOffset);
        }
        if (atkinsonDitherer)
          atkinsonDitherer->nextRow();
        else if (fsDitherer)
          fsDitherer->nextRow();
      }
      if (checkedOutput.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) success = false;
    } else {
      // Area-averaging scaling (same as JpegToBmpConverter)
      for (int outX = 0; outX < outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < static_cast<int>(width); srcX++) {
          sum += grayRow[srcX];
          count++;
        }

        if (count == 0 && srcXStart < static_cast<int>(width)) {
          sum = grayRow[srcXStart];
          count = 1;
        }

        rowAccum[outX] += sum;
        rowCount[outX] += count;
      }

      // Check if we've crossed into the next output row(s)
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

      // Output all rows whose boundaries we've crossed (handles both up and downscaling)
      // For upscaling, one source row may produce multiple output rows
      while (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, currentOutY);
            const int byteIndex = x / 8;
            const int bitOffset = 7 - (x % 8);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, currentOutY);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }

        if (checkedOutput.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
          success = false;
          break;
        }
        currentOutY++;

        nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;

        // For upscaling: don't reset accumulators if next output row uses same source data
        // Only reset when we'll move to a new source row
        if (srcY_fp >= nextOutY_srcStart) {
          // More output rows to emit from same source - keep accumulator data
          continue;
        }
        // Moving to next source row - reset accumulators
        memset(rowAccum, 0, outWidth * sizeof(uint32_t));
        memset(rowCount, 0, outWidth * sizeof(uint16_t));
      }
    }

    // Swap current/previous row buffers
    uint8_t* temp = ctx.previousRow;
    ctx.previousRow = ctx.currentRow;
    ctx.currentRow = temp;
  }

  if (success) success = finishDecodedPng(ctx);

  // Clean up
  free(grayRow);
  delete[] rowAccum;
  delete[] rowCount;
  delete atkinsonDitherer;
  delete fsDitherer;
  delete atkinson1BitDitherer;
  free(rowBuffer);
  free(ctx.currentRow);
  free(ctx.previousRow);

  if (success) {
    LOG_DBG("PNG", "Successfully converted PNG to BMP");
  }
  return success;
}

bool PngToBmpConverter::pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetWidth, targetHeight, false, crop);
}

bool PngToBmpConverter::pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight, const bool crop) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, false, crop);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight, const bool crop) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, crop);
}

bool PngToBmpConverter::pngFileToBgraBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, const int targetMaxWidth,
                                                       const int targetMaxHeight) {
  PngDecodeContext ctx = {};
  PngTransparency transparency = {};
  if (!prepareBgraDecoder(pngFile, ctx, transparency)) {
    LOG_ERR("PNG", "Invalid or unsupported alpha PNG");
    return false;
  }

  uint32_t outputWidth;
  uint32_t outputHeight;
  if (!calculateFitDimensions(ctx.width, ctx.height, targetMaxWidth, targetMaxHeight, outputWidth, outputHeight)) {
    return false;
  }

  ctx.currentRow = static_cast<uint8_t*>(malloc(ctx.rawRowBytes));
  ctx.previousRow = static_cast<uint8_t*>(calloc(ctx.rawRowBytes, 1));
  auto* outputRow = static_cast<uint8_t*>(malloc(outputWidth * 4U));
  if (!ctx.currentRow || !ctx.previousRow || !outputRow) {
    free(outputRow);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  bool success = ctx.reader.init(true);
  if (success) {
    ctx.reader.setFill(pngIdatFillCallback, &ctx);
    ctx.reader.setZlibWrapped();
    success = writeBgraBmpHeader(bmpOut, outputWidth, outputHeight);
  }

  uint32_t outputY = 0;
  for (uint32_t sourceY = 0; success && sourceY < ctx.height; ++sourceY) {
    if (!decodeScanline(ctx)) {
      success = false;
      break;
    }

    const uint32_t wantedSourceY =
        outputY < outputHeight ? static_cast<uint32_t>(static_cast<uint64_t>(outputY) * ctx.height / outputHeight)
                               : ctx.height;
    if (sourceY == wantedSourceY) {
      for (uint32_t outputX = 0; outputX < outputWidth; ++outputX) {
        const uint32_t sourceX = static_cast<uint32_t>(static_cast<uint64_t>(outputX) * ctx.width / outputWidth);
        if (!pixelToBgra(ctx, transparency, sourceX, outputRow + outputX * 4U)) {
          success = false;
          break;
        }
      }
      if (success && bmpOut.write(outputRow, outputWidth * 4U) != outputWidth * 4U) success = false;
      ++outputY;
    }

    uint8_t* previous = ctx.previousRow;
    ctx.previousRow = ctx.currentRow;
    ctx.currentRow = previous;
    if ((sourceY & 15U) == 15U) serviceLongPngConversion();
  }

  if (success && outputY == outputHeight) {
    success = finishDecodedPng(ctx);
  } else {
    success = false;
  }

  free(outputRow);
  free(ctx.currentRow);
  free(ctx.previousRow);
  return success;
}
