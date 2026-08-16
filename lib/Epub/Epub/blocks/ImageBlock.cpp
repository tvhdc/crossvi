#include "ImageBlock.h"

#include <BufferedFile.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <new>

#include "Epub/BoundedFileReader.h"
#include "Epub/SectionCacheValidator.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "PixelCacheValidation.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& sourcePath, const int16_t width,
                       const int16_t height)
    : imagePath(imagePath),
      sourcePath(sourcePath.size() <= SectionCacheValidation::MAX_IMAGE_PATH_BYTES ? sourcePath : std::string{}),
      width(width),
      height(height) {}

void* ImageBlock::extractContext = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* context, const ExtractFn extractor) {
  extractContext = context;
  extractFn = extractor;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath, const int width, const int height) {
  // Include rendered dimensions so returning to a previous font/margin layout
  // can reuse its cache instead of replacing and re-decoding one shared .pxc.
  size_t dotPos = imagePath.rfind('.');
  const std::string base = dotPos == std::string::npos ? imagePath : imagePath.substr(0, dotPos);
  return base + "_" + std::to_string(width) + "x" + std::to_string(height) + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  return pixel_cache_validation::valid(cachedWidth, cachedHeight, expectedWidth, expectedHeight,
                                       cacheFile.fileSize64());
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

// Keep small image caches in RAM across the repeated BW/LSB/MSB page passes.
// The budget is global, so a page with many images cannot multiply this cost.
constexpr size_t MAX_RESIDENT_IMAGE_CACHE_BYTES = 24U * 1024U;
size_t residentImageCacheBytes = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

void renderCacheRow(DirectPixelWriter& writer, const uint8_t* rowBuffer, const int x, const int y, const int row,
                    const int width) {
  writer.beginRow(y + row);
  int colStart = 0;
  int colEnd = 0;
  writer.bandColRange(x, width, colStart, colEnd);
  for (int col = colStart; col < colEnd; ++col) {
    const int byteIndex = col >> 2;
    const int bitShift = 6 - (col & 3) * 2;
    writer.writePixel(x + col, static_cast<uint8_t>((rowBuffer[byteIndex] >> bitShift) & 0x03U));
  }
}

void renderResidentPixels(GfxRenderer& renderer, const uint8_t* pixels, const int x, const int y, const int width,
                          const int height) {
  const int bytesPerRow = (width + 3) / 4;
  DirectPixelWriter writer;
  writer.init(renderer);
  for (int row = 0; row < height; ++row) {
    renderCacheRow(writer, pixels + static_cast<size_t>(row) * bytesPerRow, x, y, row, width);
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, std::unique_ptr<uint8_t[]>& readBuffer, size_t& readBufferCapacity,
                     std::unique_ptr<uint8_t[]>& residentPixels, size_t& residentPixelBytes, uint16_t& residentWidth,
                     uint16_t& residentHeight) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t cacheRenderStartedMs = static_cast<uint32_t>(millis());
#endif
  if (residentPixels && residentPixelBytes > 0 && residentWidth > 0 && residentHeight > 0) {
    renderResidentPixels(renderer, residentPixels.get(), x, y, residentWidth, residentHeight);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const size_t payloadBytes = static_cast<size_t>(bytesPerRow) * cachedHeight;
  if (payloadBytes <= MAX_RESIDENT_IMAGE_CACHE_BYTES &&
      payloadBytes <= MAX_RESIDENT_IMAGE_CACHE_BYTES - residentImageCacheBytes) {
    auto candidate = makeUniqueNoThrow<uint8_t[]>(payloadBytes);
    if (candidate) {
      size_t bytesRead = 0;
      while (bytesRead < payloadBytes) {
        const size_t chunk = std::min<size_t>(4096, payloadBytes - bytesRead);
        const int read = cacheFile.read(candidate.get() + bytesRead, chunk);
        if (read != static_cast<int>(chunk)) break;
        bytesRead += chunk;
      }
      const bool closed = cacheFile.close();
      if (bytesRead != payloadBytes || !closed) return false;
      residentImageCacheBytes += payloadBytes;
      residentPixelBytes = payloadBytes;
      residentWidth = cachedWidth;
      residentHeight = cachedHeight;
      residentPixels = std::move(candidate);
      renderResidentPixels(renderer, residentPixels.get(), x, y, residentWidth, residentHeight);
      LOG_DBG("IMG", "Resident cache render: %s bytes=%u", cachePath.c_str(),
              static_cast<unsigned>(residentPixelBytes));
      return true;
    }
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  const size_t requestedBytes = static_cast<size_t>(rowsPerRead) * bytesPerRow;
  if (readBufferCapacity < requestedBytes) {
    // Release a smaller buffer before growing it so page-scoped reuse never
    // raises the peak allocation above the old one-buffer render path.
    readBuffer.reset();
    readBufferCapacity = 0;
    readBuffer.reset(new (std::nothrow) uint8_t[requestedBytes]);
    if (readBuffer) readBufferCapacity = requestedBytes;
  }
  if (readBufferCapacity < static_cast<size_t>(bytesPerRow)) {
    readBuffer.reset(new (std::nothrow) uint8_t[bytesPerRow]);
    readBufferCapacity = readBuffer ? static_cast<size_t>(bytesPerRow) : 0;
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }
  rowsPerRead = std::min(static_cast<int>(cachedHeight), static_cast<int>(readBufferCapacity / bytesPerRow));

  DirectPixelWriter writer;
  writer.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer.get(), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer.get() + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    renderCacheRow(writer, rowBuffer, x, y, row, cachedWidth);
  }

  const bool closed = cacheFile.close();

  LOG_DBG("IMG", "Cache render complete: %s elapsed_ms=%u rows_per_read=%d", cachePath.c_str(),
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - cacheRenderStartedMs),
#else
          0U,
#endif
          rowsPerRead);
  return closed;
}

}  // namespace

ImageBlock::~ImageBlock() {
  if (residentPixelBytes == 0) return;
  residentImageCacheBytes = residentPixelBytes <= residentImageCacheBytes ? residentImageCacheBytes - residentPixelBytes
                                                                          : 0;
}

bool ImageBlock::hasValidCache() const {
  if (residentPixels && residentPixelBytes > 0 && residentWidth > 0 && residentHeight > 0) return true;
  const auto& cachePath = getPixelCachePath();
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const {
  return !decodedWithoutCache && !renderFailed && !imageFailedThisSession(imagePath) && !hasValidCache();
}

bool ImageBlock::preparePixelCache(GfxRenderer& renderer, const int x, const int y) const {
  if (hasValidCache()) return true;
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > renderer.getScreenWidth() ||
      y + height > renderer.getScreenHeight()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) return false;
  const size_t fileSize = file.size();
  const bool closed = file.close();
  if (fileSize == 0 || !closed) return false;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) return false;

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cacheOnly = true;
  config.cachePath = getPixelCachePath();
  return decoder->decodeToFramebuffer(imagePath, renderer, config) && hasValidCache();
}

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

const std::string& ImageBlock::getPixelCachePath() const {
  if (pixelCachePath.empty()) pixelCachePath = getCachePath(imagePath, width, height);
  return pixelCachePath;
}

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  std::unique_ptr<uint8_t[]> readBuffer;
  size_t readBufferCapacity = 0;
  render(renderer, x, y, readBuffer, readBufferCapacity);
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, std::unique_ptr<uint8_t[]>& readBuffer,
                        size_t& readBufferCapacity) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (renderFailed || imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }
  if (decodedWithoutCache) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  // Try to render from cache first
  const std::string& cachePath = getPixelCachePath();
  if (renderFromCache(renderer, cachePath, x, y, width, height, readBuffer, readBufferCapacity, residentPixels,
                      residentPixelBytes, residentWidth, residentHeight)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t materializeStartedMs = static_cast<uint32_t>(millis());
#endif
  bool imageOpened = Storage.openFileForRead("IMG", imagePath, file);
  if (!imageOpened && !Storage.exists(imagePath.c_str()) && !sourcePath.empty() && extractFn) {
    if (!extractFn(extractContext, sourcePath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Failed to extract lazy image: %s", sourcePath.c_str());
      renderFailed = true;
      rememberImageFailure(imagePath);
      renderPlaceholder(renderer, x, y);
      return;
    }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    LOG_DBG("IMGT", "lazy_extract source=%s elapsed_ms=%u", sourcePath.c_str(),
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - materializeStartedMs));
#endif
    imageOpened = Storage.openFileForRead("IMG", imagePath, file);
  }
  if (!imageOpened) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    renderFailed = true;
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    renderFailed = true;
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    renderFailed = true;
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t decodeStartedMs = static_cast<uint32_t>(millis());
#endif
  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("IMGT", "decode path=%s elapsed_ms=%u ok=%u", imagePath.c_str(),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - decodeStartedMs), success ? 1U : 0U);
#endif
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    renderFailed = true;
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  if (!hasValidCache()) {
    // The current framebuffer already contains this decode. Do not repeat an
    // expensive full-image decode in later passes of the same Page object when
    // SD publication failed; the reader will use its single-pass fallback.
    decodedWithoutCache = true;
    LOG_ERR("IMG", "Decoded image but pixel cache is unavailable: %s", imagePath.c_str());
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(serialization::BufferedFileWriter& file) {
  if (imagePath.empty() || imagePath.size() > SectionCacheValidation::MAX_IMAGE_PATH_BYTES ||
      sourcePath.size() > SectionCacheValidation::MAX_IMAGE_PATH_BYTES || width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Serialization failed: invalid image block");
    return false;
  }
  serialization::writeString(file, imagePath);
  serialization::writeString(file, sourcePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(BoundedFileReader& reader) {
  std::string path;
  std::string source;
  int16_t w = 0;
  int16_t h = 0;
  if (!reader.readString(path, SectionCacheValidation::MAX_IMAGE_PATH_BYTES, false) ||
      !reader.readString(source, SectionCacheValidation::MAX_IMAGE_PATH_BYTES, true) || !reader.readPod(w) ||
      !reader.readPod(h) || w <= 0 || h <= 0) {
    LOG_ERR("IMG", "Deserialization failed: invalid image block");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, source, w, h));
}
