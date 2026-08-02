#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

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

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr size_t MAX_BATCH_OUTPUT_WORKSPACE = 8 * 1024;
constexpr uint64_t MAX_BATCH_PACKED_BYTES = 64 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

constexpr int scaledDimension(const int dimension, const int denominator) {
  return (dimension + denominator - 1) / denominator;
}

// Use JPEGDEC's coarse downscaling whenever its output is still at least as
// large as the requested BMP. Fine scaling and dithering then process far fewer
// pixels without upscaling the decoder output.
constexpr int chooseDecodeScaleDenominator(const int srcWidth, const int srcHeight, const int outWidth,
                                           const int outHeight) {
  if (scaledDimension(srcWidth, 8) >= outWidth && scaledDimension(srcHeight, 8) >= outHeight) return 8;
  if (scaledDimension(srcWidth, 4) >= outWidth && scaledDimension(srcHeight, 4) >= outHeight) return 4;
  if (scaledDimension(srcWidth, 2) >= outWidth && scaledDimension(srcHeight, 2) >= outHeight) return 2;
  return 1;
}

static_assert(chooseDecodeScaleDenominator(1594, 2419, 90, 150) == 8);
static_assert(chooseDecodeScaleDenominator(1594, 2419, 210, 320) == 4);
static_assert(chooseDecodeScaleDenominator(1594, 2419, 500, 800) == 2);
static_assert(chooseDecodeScaleDenominator(1594, 2419, 1000, 1500) == 1);

int jpegScaleOption(const int denominator) {
  switch (denominator) {
    case 8:
      return JPEG_SCALE_EIGHTH;
    case 4:
      return JPEG_SCALE_QUARTER;
    case 2:
      return JPEG_SCALE_HALF;
    default:
      return 0;
  }
}

struct JpegInput {
  HalFile* file = nullptr;
  uint64_t offset = 0;
  uint32_t length = 0;
};

// Static bridge for JPEGDEC's C callbacks. Safe in this firmware's
// single-decoder context; the bounded range prevents a stored ZIP entry from
// exposing bytes belonging to the following entry or central directory.
static JpegInput* s_jpegInput = nullptr;

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegInput || !s_jpegInput->file || !*s_jpegInput->file || s_jpegInput->length == 0 ||
      s_jpegInput->length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      !s_jpegInput->file->seek64(s_jpegInput->offset)) {
    return nullptr;
  }
  *size = static_cast<int32_t>(s_jpegInput->length);
  return s_jpegInput;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* input = reinterpret_cast<JpegInput*>(pFile->fHandle);
  if (!input || !input->file || len <= 0 || pFile->iPos < 0 || static_cast<uint32_t>(pFile->iPos) >= input->length) {
    return 0;
  }
  const uint32_t remaining = input->length - static_cast<uint32_t>(pFile->iPos);
  const size_t requested = std::min<size_t>(static_cast<size_t>(len), remaining);
  int32_t n = input->file->read(pBuf, requested);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* input = reinterpret_cast<JpegInput*>(pFile->fHandle);
  if (!input || !input->file || pos < 0 || static_cast<uint32_t>(pos) > input->length ||
      input->offset > std::numeric_limits<uint64_t>::max() - static_cast<uint32_t>(pos) ||
      !input->file->seek64(input->offset + static_cast<uint32_t>(pos))) {
    return -1;
  }
  pFile->iPos = pos;
  return pos;
}

struct BmpOutputCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  int rowsWritten;
  bool error;
};

// Context passed to the JPEGDEC draw callback via setUserPointer(). One decoded
// MCU row is shared by up to two independent output pipelines.
struct BmpConvertCtx {
  int srcWidth;
  int srcHeight;
  std::unique_ptr<uint8_t[]> mcuBuf;
  std::array<BmpOutputCtx, JpegToBmpConverter::MAX_ONE_BIT_OUTPUTS> outputs;
  size_t outputCount;
  bool error;
};

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpOutputCtx* ctx, const uint8_t* srcRow, int outY) {
  if (ctx->error) return;
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow) != static_cast<size_t>(ctx->bytesPerRow)) {
    ctx->error = true;
    return;
  }
  ctx->rowsWritten++;
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpOutputCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpOutputCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpOutputCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpOutputCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpOutputCtx* ctx) {
  if (ctx->error) return;
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow) != static_cast<size_t>(ctx->bytesPerRow)) {
    ctx->error = true;
    return;
  }
  ctx->rowsWritten++;
  ctx->currentOutY++;
}

static void processSourceRow(BmpOutputCtx* ctx, const uint8_t* srcRow, const int y) {
  if (ctx->error) return;
  if (ctx->smoothUpscale) {
    processSmoothSourceRow(ctx, srcRow, y);
    return;
  }
  if (!ctx->needsScaling) {
    writeOutputRow(ctx, srcRow, y);
    return;
  }

  // Fixed-point area averaging on X axis.
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
    const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
    int sum = 0;
    int count = 0;
    for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
      sum += srcRow[srcX];
      count++;
    }
    if (count == 0 && srcXStart < ctx->srcWidth) {
      sum = srcRow[srcXStart];
      count = 1;
    }
    ctx->rowAccum[outX] += sum;
    ctx->rowCount[outX] += count;
  }

  // Flush output row(s) whose Y boundary we've crossed.
  const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
  while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight && !ctx->error) {
    flushScaledRow(ctx);
    ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
    if (srcY_fp >= ctx->nextOutY_srcStart) continue;
    memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
    memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
  }
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;
    for (size_t outputIndex = 0; outputIndex < ctx->outputCount; outputIndex++) {
      processSourceRow(&ctx->outputs[outputIndex], srcRow, y);
    }
  }

  if (ctx->error) return 0;
  for (size_t outputIndex = 0; outputIndex < ctx->outputCount; outputIndex++) {
    if (!ctx->outputs[outputIndex].error) return 1;
  }
  return 0;
}

struct BmpTargetSpec {
  Print* output = nullptr;
  int targetWidth = 0;
  int targetHeight = 0;
  bool oneBit = false;
  bool crop = true;
  int outWidth = 0;
  int outHeight = 0;
};

bool calculateOutputDimensions(BmpTargetSpec& target, const int srcWidth, const int srcHeight) {
  if (!target.output) return false;
  target.outWidth = srcWidth;
  target.outHeight = srcHeight;
  if (target.targetWidth <= 0 || target.targetHeight <= 0 ||
      (srcWidth == target.targetWidth && srcHeight == target.targetHeight)) {
    return true;
  }

  const float scaleToFitWidth = static_cast<float>(target.targetWidth) / srcWidth;
  const float scaleToFitHeight = static_cast<float>(target.targetHeight) / srcHeight;
  const float scale =
      target.crop ? std::max(scaleToFitWidth, scaleToFitHeight) : std::min(scaleToFitWidth, scaleToFitHeight);
  const double scaledWidth = static_cast<double>(srcWidth) * scale;
  const double scaledHeight = static_cast<double>(srcHeight) * scale;
  if (scaledWidth > std::numeric_limits<int>::max() || scaledHeight > std::numeric_limits<int>::max()) return false;
  target.outWidth = std::max(1, static_cast<int>(scaledWidth));
  target.outHeight = std::max(1, static_cast<int>(scaledHeight));
  return true;
}

uint64_t outputWorkspaceBytes(const BmpTargetSpec& target, const int scaleSrcWidth, const int scaleSrcHeight,
                              const bool progressiveDecode) {
  const bool needsScaling = scaleSrcWidth != target.outWidth || scaleSrcHeight != target.outHeight;
  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= target.outWidth && scaleSrcHeight <= target.outHeight;
  uint64_t bytesPerRow = 0;
  if (USE_8BIT_OUTPUT && !target.oneBit) {
    bytesPerRow = (static_cast<uint64_t>(target.outWidth) + 3U) / 4U * 4U;
  } else if (target.oneBit) {
    bytesPerRow = (static_cast<uint64_t>(target.outWidth) + 31U) / 32U * 4U;
  } else {
    bytesPerRow = (static_cast<uint64_t>(target.outWidth) * 2U + 31U) / 32U * 4U;
  }

  uint64_t workspace = bytesPerRow;
  if (smoothUpscale) {
    workspace += static_cast<uint64_t>(target.outWidth) * 3U;
  } else if (needsScaling) {
    workspace += static_cast<uint64_t>(target.outWidth) * sizeof(uint32_t) * 2U;
  }
  if (target.oneBit) {
    workspace += static_cast<uint64_t>(target.outWidth + 4) * sizeof(int16_t) * 3U;
  } else if (!USE_8BIT_OUTPUT && (USE_ATKINSON || USE_FLOYD_STEINBERG)) {
    workspace += static_cast<uint64_t>(target.outWidth + 4) * sizeof(int16_t) * 3U;
  }
  return workspace;
}

bool initialiseOutput(BmpOutputCtx& ctx, const BmpTargetSpec& target, const int scaleSrcWidth, const int scaleSrcHeight,
                      const bool progressiveDecode) {
  const uint64_t bytesPerRow64 = USE_8BIT_OUTPUT && !target.oneBit
                                     ? (static_cast<uint64_t>(target.outWidth) + 3U) / 4U * 4U
                                 : target.oneBit ? (static_cast<uint64_t>(target.outWidth) + 31U) / 32U * 4U
                                                 : (static_cast<uint64_t>(target.outWidth) * 2U + 31U) / 32U * 4U;
  if (bytesPerRow64 > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
  const int bytesPerRow = static_cast<int>(bytesPerRow64);
  const bool needsScaling = scaleSrcWidth != target.outWidth || scaleSrcHeight != target.outHeight;
  const uint32_t scaleX_fp = needsScaling ? (static_cast<uint32_t>(scaleSrcWidth) << 16) / target.outWidth : FP_ONE;
  const uint32_t scaleY_fp = needsScaling ? (static_cast<uint32_t>(scaleSrcHeight) << 16) / target.outHeight : FP_ONE;
  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= target.outWidth && scaleSrcHeight <= target.outHeight;

  ctx.bmpOut = target.output;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = target.outWidth;
  ctx.outHeight = target.outHeight;
  ctx.oneBit = target.oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(scaleSrcWidth, target.outWidth);
  ctx.smoothScaleY_fp = interpolationStep(scaleSrcHeight, target.outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.error = false;

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    const size_t smoothRowsBytes = static_cast<size_t>(target.outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + target.outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + target.outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", scaleSrcWidth, scaleSrcHeight,
            target.outWidth, target.outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(target.outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(target.outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (target.oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(target.outWidth, std::nothrow);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->valid()) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(target.outWidth, std::nothrow);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->valid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(target.outWidth, std::nothrow);
      if (!ctx.fsDitherer || !ctx.fsDitherer->valid()) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  // Do not leave a plausible partial bitmap behind when any working
  // allocation fails.
  if (USE_8BIT_OUTPUT && !target.oneBit) {
    writeBmpHeader8bit(*target.output, target.outWidth, target.outHeight);
  } else if (target.oneBit) {
    writeBmpHeader1bit(*target.output, target.outWidth, target.outHeight);
  } else {
    writeBmpHeader2bit(*target.output, target.outWidth, target.outHeight);
  }
  return true;
}

uint8_t convertJpegToBmpStreams(HalFile& jpegFile, const uint64_t sourceOffset, const uint32_t sourceLength,
                                BmpTargetSpec* targets, const size_t targetCount) {
  if (!targets || targetCount == 0 || targetCount > JpegToBmpConverter::MAX_ONE_BIT_OUTPUTS) return 0;
  if (!jpegFile || sourceLength == 0 || sourceLength > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    return 0;
  }
  const uint64_t fileSize = jpegFile.fileSize64();
  if (sourceOffset > fileSize || sourceLength > fileSize - sourceOffset) {
    return 0;
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return 0;
  }

  JpegInput input{&jpegFile, sourceOffset, sourceLength};
  s_jpegInput = &input;

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    s_jpegInput = nullptr;
    return 0;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    s_jpegInput = nullptr;
    return 0;
  }

  const ScopedCleanup cleanup{[&jpeg]() {
    jpeg->close();
    s_jpegInput = nullptr;
  }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return 0;
  }

  for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
    if (!calculateOutputDimensions(targets[outputIndex], srcWidth, srcHeight)) return 0;
  }

  // JPEGDEC always decodes progressive images at 1/8 resolution. Baseline
  // images use the highest-resolution requirement among every requested BMP.
  int decodeScaleDenominator = 8;
  if (!progressiveDecode) {
    for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
      decodeScaleDenominator = std::min(decodeScaleDenominator,
                                        chooseDecodeScaleDenominator(srcWidth, srcHeight, targets[outputIndex].outWidth,
                                                                     targets[outputIndex].outHeight));
    }
  }
  const int decodeScaleOption = jpegScaleOption(decodeScaleDenominator);
  const int scaleSrcWidth = scaledDimension(srcWidth, decodeScaleDenominator);
  const int scaleSrcHeight = scaledDimension(srcHeight, decodeScaleDenominator);

  uint64_t batchWorkspace = 0;
  for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
    BmpTargetSpec& target = targets[outputIndex];
    if (target.targetWidth <= 0 || target.targetHeight <= 0) {
      target.outWidth = scaleSrcWidth;
      target.outHeight = scaleSrcHeight;
    }
    LOG_DBG("JPG", "Scaling source %dx%d (JPEGDEC 1/%d grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight,
            decodeScaleDenominator, scaleSrcWidth, scaleSrcHeight, target.outWidth, target.outHeight,
            target.targetWidth, target.targetHeight);

    if (targetCount > 1) {
      const uint64_t bytesPerRow = (static_cast<uint64_t>(target.outWidth) + 31U) / 32U * 4U;
      const uint64_t packedBytes = bytesPerRow * static_cast<uint64_t>(target.outHeight);
      if (!target.oneBit || packedBytes == 0 || packedBytes > MAX_BATCH_PACKED_BYTES) {
        LOG_ERR("JPG", "Batch output dimensions exceed thumbnail limits");
        return 0;
      }
      batchWorkspace += outputWorkspaceBytes(target, scaleSrcWidth, scaleSrcHeight, progressiveDecode);
      if (batchWorkspace > MAX_BATCH_OUTPUT_WORKSPACE) {
        LOG_ERR("JPG", "Batch output workspace exceeds %u bytes", static_cast<unsigned>(MAX_BATCH_OUTPUT_WORKSPACE));
        return 0;
      }
    }
  }

  BmpConvertCtx ctx = {};
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outputCount = targetCount;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return 0;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
    if (!initialiseOutput(ctx.outputs[outputIndex], targets[outputIndex], scaleSrcWidth, scaleSrcHeight,
                          progressiveDecode)) {
      ctx.outputs[outputIndex].error = true;
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, decodeScaleOption);

  if (rc == 1 && !ctx.error) {
    for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
      BmpOutputCtx& output = ctx.outputs[outputIndex];
      if (output.smoothUpscale && !output.error) finishSmoothUpscale(&output);
    }
  }

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    return 0;
  }

  uint8_t result = 0;
  size_t successCount = 0;
  for (size_t outputIndex = 0; outputIndex < targetCount; outputIndex++) {
    const BmpOutputCtx& output = ctx.outputs[outputIndex];
    if (!output.error && output.rowsWritten == output.outHeight) {
      result |= static_cast<uint8_t>(1U << outputIndex);
      successCount++;
    }
  }
  LOG_DBG("JPG", "Converted JPEG to %u/%u BMP streams", static_cast<unsigned>(successCount),
          static_cast<unsigned>(targetCount));
  return result;
}

}  // namespace

// Internal implementation with configurable target size and bit depth.
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, const uint64_t sourceOffset,
                                                     const uint32_t sourceLength, Print& bmpOut, const int targetWidth,
                                                     const int targetHeight, const bool oneBit, const bool crop) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);
  BmpTargetSpec target{&bmpOut, targetWidth, targetHeight, oneBit, crop};
  return convertJpegToBmpStreams(jpegFile, sourceOffset, sourceLength, &target, 1) == 1U;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  const uint64_t length = jpegFile.fileSize64();
  if (length == 0 || length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) return false;
  return jpegFileToBmpStreamInternal(jpegFile, 0, static_cast<uint32_t>(length), bmpOut, targetWidth, targetHeight,
                                     false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  const uint64_t length = jpegFile.fileSize64();
  if (length == 0 || length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) return false;
  return jpegFileToBmpStreamInternal(jpegFile, 0, static_cast<uint32_t>(length), bmpOut, targetMaxWidth,
                                     targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight, const bool crop) {
  const uint64_t length = jpegFile.fileSize64();
  if (length == 0 || length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) return false;
  return jpegFileToBmpStreamInternal(jpegFile, 0, static_cast<uint32_t>(length), bmpOut, targetMaxWidth,
                                     targetMaxHeight, true, crop);
}

bool JpegToBmpConverter::jpegFileRangeToBmpStream(HalFile& jpegFile, const uint64_t sourceOffset,
                                                  const uint32_t sourceLength, Print& bmpOut, const bool crop) {
  return jpegFileToBmpStreamInternal(jpegFile, sourceOffset, sourceLength, bmpOut, display.getDisplayHeight(),
                                     display.getDisplayWidth(), false, crop);
}

bool JpegToBmpConverter::jpegFileRangeTo1BitBmpStreamWithSize(HalFile& jpegFile, const uint64_t sourceOffset,
                                                              const uint32_t sourceLength, Print& bmpOut,
                                                              const int targetMaxWidth, const int targetMaxHeight,
                                                              const bool crop) {
  return jpegFileToBmpStreamInternal(jpegFile, sourceOffset, sourceLength, bmpOut, targetMaxWidth, targetMaxHeight,
                                     true, crop);
}

uint8_t JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSize(HalFile& jpegFile, const OneBitBmpTarget* targets,
                                                             const size_t targetCount) {
  const uint64_t length = jpegFile.fileSize64();
  if (length == 0 || length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) return 0;
  return jpegFileRangeTo1BitBmpStreamsWithSize(jpegFile, 0, static_cast<uint32_t>(length), targets, targetCount);
}

uint8_t JpegToBmpConverter::jpegFileRangeTo1BitBmpStreamsWithSize(HalFile& jpegFile, const uint64_t sourceOffset,
                                                                  const uint32_t sourceLength,
                                                                  const OneBitBmpTarget* targets,
                                                                  const size_t targetCount) {
  if (!targets || targetCount == 0 || targetCount > MAX_ONE_BIT_OUTPUTS) return 0;
  std::array<BmpTargetSpec, MAX_ONE_BIT_OUTPUTS> specs{};
  for (size_t index = 0; index < targetCount; index++) {
    if (!targets[index].output || targets[index].targetMaxWidth <= 0 || targets[index].targetMaxHeight <= 0) return 0;
    specs[index] = {targets[index].output, targets[index].targetMaxWidth, targets[index].targetMaxHeight, true,
                    targets[index].crop};
  }
  return convertJpegToBmpStreams(jpegFile, sourceOffset, sourceLength, specs.data(), targetCount);
}
