/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossVi
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <StagedFileTransaction.h>

#include <algorithm>

namespace {
bool publishBitmap(const std::string& finalPath, const std::string& stagingPath) {
  const std::string backupPath = finalPath + ".bak";
  return StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                        Bitmap::validateFile, nullptr) == StagedFileTransaction::Status::Published;
}

bool exactWrite(HalFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }
}  // namespace

bool Xtc::load() {
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());
  loaded = false;

  // Initialize parser
  parser = makeUniqueNoThrow<xtc::XtcParser>();
  if (!parser) {
    LOG_ERR("XTC", "Failed to allocate parser");
    return false;
  }

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("XTC", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  LOG_DBG("XTC", "Cache cleared successfully");
  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

const std::vector<xtc::ChapterInfo>& Xtc::getChapters() {
  static const std::vector<xtc::ChapterInfo> kEmpty;
  if (!loaded || !parser) {
    return kEmpty;
  }
  return parser->getChapters();
}

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::generateCoverBmp() const {
  const std::string finalPath = getCoverBmpPath();
  const std::string stagingPath = finalPath + ".tmp";
  const BitmapCacheState cacheState = Bitmap::inspectDerivedCache(finalPath);
  if (cacheState == BitmapCacheState::Ready) return true;
  if (cacheState == BitmapCacheState::IoError) return false;

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();

  xtc::PageLayout pageLayout;
  if (!xtc::calculatePageLayout(pageInfo.width, pageInfo.height, bitDepth, pageLayout)) return false;
  const size_t bitmapSize = pageLayout.payloadBytes;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  // Load first page (cover)
  const size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    LOG_ERR("XTC", "Failed to load cover page");
    free(pageBuffer);
    return false;
  }

  const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;
  const size_t dstRowSize = (pageInfo.width + 7) / 8;  // 1-bit destination row size
  uint8_t* rowBuffer = bitDepth == 2 ? static_cast<uint8_t*>(malloc(dstRowSize)) : nullptr;
  if (bitDepth == 2 && !rowBuffer) {
    free(pageBuffer);
    return false;
  }

  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) {
    free(rowBuffer);
    free(pageBuffer);
    return false;
  }
  HalFile coverBmp;
  if (!Storage.openFileForWrite("XTC", stagingPath, coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP staging file");
    free(rowBuffer);
    free(pageBuffer);
    return false;
  }

  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
  bool writeOk = exactWrite(coverBmp, &bmpHeader, sizeof(bmpHeader));
  const uint8_t padding[4] = {0, 0, 0, 0};

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit0, second plane: Bit1
    // - Pixel value = bit0 | (bit1 << 1)
    for (uint16_t y = 0; writeOk && y < pageInfo.height; y++) {
      memset(rowBuffer, 0xFF, dstRowSize);  // Start with all white

      for (uint16_t x = 0; x < pageInfo.width; x++) {
        const uint8_t pixelValue = xtc::readXthPixel(pageBuffer, pageLayout, pageInfo.width, x, y);

        // Threshold: 0=white (1); 1,2,3=black (0)
        if (pixelValue >= 1) {
          // Set bit to 0 (black) in BMP format
          const size_t dstByte = x / 8;
          const size_t dstBit = 7 - (x % 8);
          rowBuffer[dstByte] &= ~(1 << dstBit);
        }
      }

      const size_t paddingSize = rowSize - dstRowSize;
      writeOk = exactWrite(coverBmp, rowBuffer, dstRowSize) &&
                (paddingSize == 0 || exactWrite(coverBmp, padding, paddingSize));
    }
  } else {
    const size_t srcRowSize = (pageInfo.width + 7) / 8;
    for (uint16_t y = 0; writeOk && y < pageInfo.height; y++) {
      const size_t paddingSize = rowSize - srcRowSize;
      writeOk = exactWrite(coverBmp, pageBuffer + y * srcRowSize, srcRowSize) &&
                (paddingSize == 0 || exactWrite(coverBmp, padding, paddingSize));
    }
  }

  const bool synced = coverBmp.sync();
  const bool closed = coverBmp.close();
  free(rowBuffer);
  free(pageBuffer);
  if (!writeOk || !synced || !closed || !publishBitmap(finalPath, stagingPath)) {
    Storage.remove(stagingPath.c_str());
    return false;
  }
  LOG_DBG("XTC", "Generated cover BMP: %s", finalPath.c_str());
  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Xtc::generateThumbBmp(const int height) const {
  return generateThumbBmp(static_cast<int>(height * 0.6f), height, true);
}

bool Xtc::generateThumbBmpPair(const int carouselWidth, const int carouselHeight) const {
  if (carouselWidth <= 0 || carouselHeight <= 0) return false;

  const std::string sharedPath = getThumbBmpPath(SHARED_THUMB_HEIGHT);
  const std::string carouselPath = getThumbBmpPath(carouselHeight);
  const auto cacheReady = [](const std::string& finalPath, const int width, const int height, const bool crop,
                             bool& ioError) {
    const BitmapCacheState state = Bitmap::inspectDerivedCache(finalPath);
    if (state == BitmapCacheState::IoError) {
      ioError = true;
      return false;
    }
    if (state != BitmapCacheState::Ready || crop) return state == BitmapCacheState::Ready;

    HalFile cached;
    if (!Storage.openFileForRead("XTC", finalPath, cached)) {
      ioError = true;
      return false;
    }
    Bitmap bitmap(cached);
    const bool fits = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.is1Bit() && bitmap.getWidth() > 0 &&
                      bitmap.getHeight() > 0 && bitmap.getWidth() <= width && bitmap.getHeight() <= height;
    const bool closed = cached.close();
    if (fits && closed) return true;
    if (!closed || !Storage.remove(finalPath.c_str())) ioError = true;
    return false;
  };

  bool sharedIoError = false;
  bool carouselIoError = false;
  const bool sharedReady = cacheReady(sharedPath, SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT, true, sharedIoError);
  const bool carouselReady = cacheReady(carouselPath, carouselWidth, carouselHeight, false, carouselIoError);
  if (sharedIoError || carouselIoError) return false;
  if (sharedReady && carouselReady) return true;

  if (!loaded || !parser || parser->getPageCount() == 0) return false;
  setupCacheDir();

  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) return false;
  const uint8_t bitDepth = parser->getBitDepth();
  xtc::PageLayout pageLayout;
  if (!xtc::calculatePageLayout(pageInfo.width, pageInfo.height, bitDepth, pageLayout)) return false;

  const size_t bitmapSize = pageLayout.payloadBytes;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) return false;
  const size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    free(pageBuffer);
    return false;
  }

  const int largestWidth = std::max({static_cast<int>(pageInfo.width), SHARED_THUMB_WIDTH, carouselWidth});
  const size_t rowBufferSize = (static_cast<size_t>(largestWidth) + 31U) / 32U * 4U;
  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowBufferSize));
  if (!rowBuffer) {
    free(pageBuffer);
    return false;
  }

  const auto writeThumbnail = [&](const std::string& finalPath, const int targetWidth, const int targetHeight,
                                  const bool crop) {
    float scaleX = static_cast<float>(targetWidth) / pageInfo.width;
    float scaleY = static_cast<float>(targetHeight) / pageInfo.height;
    float scale = crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    scale = std::min(scale, 1.0f);
    if (scale <= 0.0f) return false;

    const uint16_t thumbWidth = std::max<uint16_t>(1, static_cast<uint16_t>(pageInfo.width * scale));
    const uint16_t thumbHeight = std::max<uint16_t>(1, static_cast<uint16_t>(pageInfo.height * scale));
    const size_t rowSize = (static_cast<size_t>(thumbWidth) + 31U) / 32U * 4U;
    if (rowSize > rowBufferSize) return false;

    const std::string stagingPath = finalPath + ".tmp";
    if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;
    HalFile output;
    if (!Storage.openFileForWrite("XTC", stagingPath, output)) return false;

    BmpHeader bmpHeader;
    createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
    bool writeOk = exactWrite(output, &bmpHeader, sizeof(bmpHeader));
    const uint32_t scaleInverse = static_cast<uint32_t>(65536.0f / scale);
    const size_t sourceRowBytes = bitDepth == 1 ? (pageInfo.width + 7U) / 8U : 0;

    for (uint16_t dstY = 0; writeOk && dstY < thumbHeight; ++dstY) {
      memset(rowBuffer, 0xFF, rowSize);
      uint32_t sourceYStart = (static_cast<uint32_t>(dstY) * scaleInverse) >> 16U;
      uint32_t sourceYEnd = (static_cast<uint32_t>(dstY + 1U) * scaleInverse) >> 16U;
      if (sourceYStart >= pageInfo.height) sourceYStart = pageInfo.height - 1U;
      if (sourceYEnd > pageInfo.height) sourceYEnd = pageInfo.height;
      if (sourceYEnd <= sourceYStart) sourceYEnd = sourceYStart + 1U;
      if (sourceYEnd > pageInfo.height) sourceYEnd = pageInfo.height;

      for (uint16_t dstX = 0; dstX < thumbWidth; ++dstX) {
        uint32_t sourceXStart = (static_cast<uint32_t>(dstX) * scaleInverse) >> 16U;
        uint32_t sourceXEnd = (static_cast<uint32_t>(dstX + 1U) * scaleInverse) >> 16U;
        if (sourceXStart >= pageInfo.width) sourceXStart = pageInfo.width - 1U;
        if (sourceXEnd > pageInfo.width) sourceXEnd = pageInfo.width;
        if (sourceXEnd <= sourceXStart) sourceXEnd = sourceXStart + 1U;
        if (sourceXEnd > pageInfo.width) sourceXEnd = pageInfo.width;

        uint32_t graySum = 0;
        uint32_t totalCount = 0;
        for (uint32_t sourceY = sourceYStart; sourceY < sourceYEnd; ++sourceY) {
          for (uint32_t sourceX = sourceXStart; sourceX < sourceXEnd; ++sourceX) {
            uint8_t grayValue = 255;
            if (bitDepth == 2) {
              const uint8_t pixelValue = xtc::readXthPixel(pageBuffer, pageLayout, pageInfo.width, sourceX, sourceY);
              grayValue = static_cast<uint8_t>((3U - pixelValue) * 85U);
            } else {
              const size_t byteIndex = sourceY * sourceRowBytes + sourceX / 8U;
              const size_t bitOffset = 7U - (sourceX % 8U);
              if (byteIndex < bitmapSize) grayValue = ((pageBuffer[byteIndex] >> bitOffset) & 1U) ? 255 : 0;
            }
            graySum += grayValue;
            ++totalCount;
          }
        }

        const uint8_t averageGray = totalCount > 0 ? static_cast<uint8_t>(graySum / totalCount) : 255;
        uint32_t hash = static_cast<uint32_t>(dstX) * 374761393U + static_cast<uint32_t>(dstY) * 668265263U;
        hash = (hash ^ (hash >> 13U)) * 1274126177U;
        const int threshold = static_cast<int>(hash >> 24U);
        const int adjustedThreshold = 128 + ((threshold - 128) / 2);
        if (averageGray < adjustedThreshold) {
          const size_t byteIndex = dstX / 8U;
          const size_t bitOffset = 7U - (dstX % 8U);
          rowBuffer[byteIndex] &= ~(1U << bitOffset);
        }
      }
      writeOk = exactWrite(output, rowBuffer, rowSize);
    }

    const bool synced = output.sync();
    const bool closed = output.close();
    const bool published = writeOk && synced && closed && publishBitmap(finalPath, stagingPath);
    if (!published) Storage.remove(stagingPath.c_str());
    return published;
  };

  const bool sharedOk = sharedReady || writeThumbnail(sharedPath, SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT, true);
  const bool carouselOk = carouselReady || writeThumbnail(carouselPath, carouselWidth, carouselHeight, false);
  free(rowBuffer);
  free(pageBuffer);
  return sharedOk && carouselOk;
}

bool Xtc::generateThumbBmp(const int width, const int height, const bool crop) const {
  const uint32_t totalStart = static_cast<uint32_t>(millis());
  LOG_DBG("COVR", "XTC thumb begin path=%s height=%d", filepath.c_str(), height);
  if (width <= 0 || height <= 0) return false;
  const std::string finalPath = getThumbBmpPath(height);
  const std::string stagingPath = finalPath + ".tmp";
  const uint32_t cacheStart = static_cast<uint32_t>(millis());
  const BitmapCacheState cacheState = Bitmap::inspectDerivedCache(finalPath);
  LOG_DBG("COVR", "XTC thumb cache_check_ms=%u state=%u",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - cacheStart), static_cast<unsigned>(cacheState));
  if (cacheState == BitmapCacheState::Ready) {
    if (crop) return true;
    HalFile cached;
    if (!Storage.openFileForRead("XTC", finalPath, cached)) return false;
    Bitmap bitmap(cached);
    const bool fits = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.is1Bit() && bitmap.getWidth() > 0 &&
                      bitmap.getHeight() > 0 && bitmap.getWidth() <= width && bitmap.getHeight() <= height;
    const bool closed = cached.close();
    if (fits && closed) return true;
    if (!closed || !Storage.remove(finalPath.c_str())) return false;
  }
  if (cacheState == BitmapCacheState::IoError) return false;

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();

  // Calculate target dimensions for thumbnail (fit within 240x400 Continue Reading card)
  const int THUMB_TARGET_WIDTH = width;
  const int THUMB_TARGET_HEIGHT = height;

  // Calculate scale factor
  float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  float scale = crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);

  // Only scale down, never up
  if (scale >= 1.0f && crop) {
    if (generateCoverBmp()) {
      HalFile src, dst;
      if (!Storage.openFileForRead("XTC", getCoverBmpPath(), src)) return false;
      if ((Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) ||
          !Storage.openFileForWrite("XTC", stagingPath, dst)) {
        src.close();
        return false;
      }
      bool copyOk = true;
      uint8_t buffer[512];
      uint64_t remaining = src.fileSize64();
      while (remaining > 0) {
        const size_t chunk = remaining < sizeof(buffer) ? static_cast<size_t>(remaining) : sizeof(buffer);
        const int bytesRead = src.read(buffer, chunk);
        if (bytesRead != static_cast<int>(chunk) || !exactWrite(dst, buffer, chunk)) {
          copyOk = false;
          break;
        }
        remaining -= chunk;
      }
      const bool synced = dst.sync();
      const bool dstClosed = dst.close();
      const bool srcClosed = src.close();
      if (!copyOk || !synced || !dstClosed || !srcClosed || !publishBitmap(finalPath, stagingPath)) {
        Storage.remove(stagingPath.c_str());
        LOG_ERR("XTC", "Failed to copy cover to thumbnail");
        return false;
      }
      LOG_DBG("XTC", "Copied cover to thumb (no scaling needed)");
      return true;
    }
    return false;
  }
  scale = std::min(scale, 1.0f);

  uint16_t thumbWidth = std::max<uint16_t>(1, static_cast<uint16_t>(pageInfo.width * scale));
  uint16_t thumbHeight = std::max<uint16_t>(1, static_cast<uint16_t>(pageInfo.height * scale));

  LOG_DBG("XTC", "Generating thumb BMP: %dx%d -> %dx%d (scale: %.3f)", pageInfo.width, pageInfo.height, thumbWidth,
          thumbHeight, scale);

  xtc::PageLayout pageLayout;
  if (!xtc::calculatePageLayout(pageInfo.width, pageInfo.height, bitDepth, pageLayout)) return false;
  const size_t bitmapSize = pageLayout.payloadBytes;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  // Load first page (cover)
  const uint32_t pageLoadStart = static_cast<uint32_t>(millis());
  const size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  LOG_DBG("COVR", "XTC first_page_load_ms=%u bytes=%u expected=%u",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - pageLoadStart), static_cast<unsigned>(bytesRead),
          static_cast<unsigned>(bitmapSize));
  if (bytesRead != bitmapSize) {
    LOG_ERR("XTC", "Failed to load cover page for thumb");
    free(pageBuffer);
    return false;
  }

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));
  if (!rowBuffer) {
    free(pageBuffer);
    return false;
  }

  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) {
    free(rowBuffer);
    free(pageBuffer);
    return false;
  }
  HalFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", stagingPath, thumbBmp)) {
    LOG_DBG("XTC", "Failed to create thumb BMP staging file");
    free(rowBuffer);
    free(pageBuffer);
    return false;
  }
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  bool writeOk = exactWrite(thumbBmp, &bmpHeader, sizeof(bmpHeader));

  // Fixed-point scale factor (16.16)
  uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);

  // Pre-calculate plane info for 2-bit mode
  const size_t srcRowBytes = (bitDepth == 1) ? ((pageInfo.width + 7) / 8) : 0;

  const uint32_t scaleStart = static_cast<uint32_t>(millis());
  for (uint16_t dstY = 0; writeOk && dstY < thumbHeight; dstY++) {
    memset(rowBuffer, 0xFF, rowSize);  // Start with all white (bit 1)

    // Calculate source Y range with bounds checking
    uint32_t srcYStart = (static_cast<uint32_t>(dstY) * scaleInv_fp) >> 16;
    uint32_t srcYEnd = (static_cast<uint32_t>(dstY + 1) * scaleInv_fp) >> 16;
    if (srcYStart >= pageInfo.height) srcYStart = pageInfo.height - 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
    if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      // Calculate source X range with bounds checking
      uint32_t srcXStart = (static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
      uint32_t srcXEnd = (static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
      if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
      if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;

      // Area averaging: sum grayscale values (0-255 range)
      uint32_t graySum = 0;
      uint32_t totalCount = 0;

      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          uint8_t grayValue = 255;  // Default: white

          if (bitDepth == 2) {
            // XTH 2-bit mode: pixel value 0-3
            // Bounds check for column index
            if (srcX < pageInfo.width) {
              const uint8_t pixelValue = xtc::readXthPixel(pageBuffer, pageLayout, pageInfo.width, srcX, srcY);
              grayValue = static_cast<uint8_t>((3U - pixelValue) * 85U);
            }
          } else {
            // 1-bit mode
            const size_t byteIdx = srcY * srcRowBytes + srcX / 8;
            const size_t bitIdx = 7 - (srcX % 8);
            // Bounds check for buffer access
            if (byteIdx < bitmapSize) {
              const uint8_t pixelBit = (pageBuffer[byteIdx] >> bitIdx) & 1;
              // XTC 1-bit polarity: 0=black, 1=white (same as BMP palette)
              grayValue = pixelBit ? 255 : 0;
            }
          }

          graySum += grayValue;
          totalCount++;
        }
      }

      // Calculate average grayscale and quantize to 1-bit with noise dithering
      uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;

      // Hash-based noise dithering for 1-bit output
      uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);           // 0-255
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);  // Range: 64-192

      // Quantize to 1-bit: 0=black, 1=white
      uint8_t oneBit = (avgGray >= adjustedThreshold) ? 1 : 0;

      // Pack 1-bit value into row buffer (MSB first, 8 pixels per byte)
      const size_t byteIndex = dstX / 8;
      const size_t bitOffset = 7 - (dstX % 8);
      // Bounds check for row buffer access
      if (byteIndex < rowSize) {
        if (oneBit) {
          rowBuffer[byteIndex] |= (1 << bitOffset);  // Set bit for white
        } else {
          rowBuffer[byteIndex] &= ~(1 << bitOffset);  // Clear bit for black
        }
      }
    }

    writeOk = exactWrite(thumbBmp, rowBuffer, rowSize);
  }
  LOG_DBG("COVR", "XTC scale_write_ms=%u output=%ux%u write_ok=%d",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - scaleStart), static_cast<unsigned>(thumbWidth),
          static_cast<unsigned>(thumbHeight), writeOk);

  const uint32_t finalizeStart = static_cast<uint32_t>(millis());
  const bool synced = thumbBmp.sync();
  const bool closed = thumbBmp.close();
  free(rowBuffer);
  free(pageBuffer);
  const bool published = writeOk && synced && closed && publishBitmap(finalPath, stagingPath);
  LOG_DBG("COVR", "XTC thumb finalize_ms=%u sync=%d close=%d publish=%d",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - finalizeStart), synced, closed, published);
  if (!published) {
    Storage.remove(stagingPath.c_str());
    return false;
  }
  LOG_DBG("COVR", "XTC thumb done total_ms=%u path=%s",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - totalStart), finalPath.c_str());
  return true;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

bool Xtc::getSourceIdentity(ZipFile::SourceIdentity& identity) const {
  return loaded && parser && parser->getSourceIdentity(identity);
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}
