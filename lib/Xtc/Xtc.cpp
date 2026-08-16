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
#include <array>
#include <cstring>
#include <limits>

namespace {
bool publishBitmap(const std::string& finalPath, const std::string& stagingPath) {
  const std::string backupPath = finalPath + ".bak";
  return StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                        Bitmap::validateFile, nullptr) == StagedFileTransaction::Status::Published;
}

bool exactWrite(HalFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }

constexpr size_t MAX_STREAMING_THUMBNAIL_BYTES = 64U * 1024U;
constexpr size_t MAX_THUMBNAIL_SOURCE_CHUNK = 1024;

bool thumbnailCacheReady(const std::string& finalPath, const int width, const int height, const bool crop,
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
}

struct StreamingThumbnail {
  uint16_t sourceWidth = 0;
  uint16_t sourceHeight = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  size_t rowSize = 0;
  uint32_t scaleInverse = 0;
  bool columnMajor = false;
  bool valid = false;
  bool finished = false;
  std::unique_ptr<uint8_t[]> pixels;
  std::unique_ptr<uint32_t[]> sums;

  int32_t activePrimary = -1;
  uint16_t lastSourcePrimary = std::numeric_limits<uint16_t>::max();
  uint16_t secondaryDestination = 0;
  bool primaryAccepted = false;
  bool primaryHasSamples = false;
  size_t finalizedPrimary = 0;

  bool initialise(const uint16_t sourceW, const uint16_t sourceH, const int targetWidth, const int targetHeight,
                  const bool crop, const bool useColumnMajor) {
    if (sourceW == 0 || sourceH == 0 || targetWidth <= 0 || targetHeight <= 0) return false;

    const float scaleX = static_cast<float>(targetWidth) / sourceW;
    const float scaleY = static_cast<float>(targetHeight) / sourceH;
    const float scale = std::min(crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY), 1.0f);
    if (scale <= 0.0f) return false;

    sourceWidth = sourceW;
    sourceHeight = sourceH;
    width = std::max<uint16_t>(1, static_cast<uint16_t>(sourceW * scale));
    height = std::max<uint16_t>(1, static_cast<uint16_t>(sourceH * scale));
    rowSize = (static_cast<size_t>(width) + 31U) / 32U * 4U;
    scaleInverse = static_cast<uint32_t>(65536.0f / scale);
    columnMajor = useColumnMajor;

    size_t pixelBytes = 0;
    if (scaleInverse == 0 || !xtc::checkedMultiply(rowSize, height, pixelBytes) || pixelBytes == 0 ||
        pixelBytes > MAX_STREAMING_THUMBNAIL_BYTES) {
      return false;
    }
    const size_t scratchLength = columnMajor ? height : width;
    pixels = makeUniqueNoThrow<uint8_t[]>(pixelBytes);
    sums = makeUniqueNoThrow<uint32_t[]>(scratchLength);
    if (!pixels || !sums) return false;
    std::memset(pixels.get(), 0xFF, pixelBytes);
    valid = true;
    return true;
  }

  void sourceRange(const uint16_t destination, const uint16_t sourceSize, uint32_t& start, uint32_t& end) const {
    start = static_cast<uint32_t>((static_cast<uint64_t>(destination) * scaleInverse) >> 16U);
    end = static_cast<uint32_t>((static_cast<uint64_t>(destination + 1U) * scaleInverse) >> 16U);
    if (start >= sourceSize) start = sourceSize - 1U;
    if (end > sourceSize) end = sourceSize;
    if (end <= start) end = start + 1U;
    if (end > sourceSize) end = sourceSize;
  }

  size_t primaryOutputSize() const { return columnMajor ? width : height; }
  size_t secondaryOutputSize() const { return columnMajor ? height : width; }
  uint16_t primarySourceSize() const { return columnMajor ? sourceWidth : sourceHeight; }
  uint16_t secondarySourceSize() const { return columnMajor ? sourceHeight : sourceWidth; }

  void clearSums() {
    std::memset(sums.get(), 0, secondaryOutputSize() * sizeof(uint32_t));
    primaryHasSamples = false;
  }

  void setBlack(const uint16_t x, const uint16_t y) {
    const size_t byteIndex = static_cast<size_t>(y) * rowSize + x / 8U;
    const uint8_t bitOffset = static_cast<uint8_t>(7U - x % 8U);
    pixels[byteIndex] &= static_cast<uint8_t>(~(1U << bitOffset));
  }

  bool finalizeActivePrimary() {
    if (!valid || activePrimary < 0 || static_cast<size_t>(activePrimary) >= primaryOutputSize() ||
        !primaryHasSamples) {
      valid = false;
      return false;
    }

    uint32_t primaryStart = 0;
    uint32_t primaryEnd = 0;
    sourceRange(static_cast<uint16_t>(activePrimary), primarySourceSize(), primaryStart, primaryEnd);
    const uint32_t primarySpan = primaryEnd - primaryStart;
    for (uint16_t secondary = 0; secondary < secondaryOutputSize(); ++secondary) {
      uint32_t secondaryStart = 0;
      uint32_t secondaryEnd = 0;
      sourceRange(secondary, secondarySourceSize(), secondaryStart, secondaryEnd);
      const uint32_t sampleCount = primarySpan * (secondaryEnd - secondaryStart);
      if (sampleCount == 0) {
        valid = false;
        return false;
      }

      const uint16_t x = columnMajor ? static_cast<uint16_t>(activePrimary) : secondary;
      const uint16_t y = columnMajor ? secondary : static_cast<uint16_t>(activePrimary);
      const uint8_t averageGray = static_cast<uint8_t>(sums[secondary] / sampleCount);
      uint32_t hash = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
      hash = (hash ^ (hash >> 13U)) * 1274126177U;
      const int threshold = static_cast<int>(hash >> 24U);
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);
      if (averageGray < adjustedThreshold) setBlack(x, y);
    }
    ++finalizedPrimary;
    return true;
  }

  void beginAscendingPrimary(const uint16_t source) {
    if (activePrimary < 0) {
      activePrimary = 0;
      clearSums();
    }
    while (valid && static_cast<size_t>(activePrimary) < primaryOutputSize()) {
      uint32_t start = 0;
      uint32_t end = 0;
      sourceRange(static_cast<uint16_t>(activePrimary), primarySourceSize(), start, end);
      if (source < end) break;
      if (!finalizeActivePrimary()) return;
      ++activePrimary;
      if (static_cast<size_t>(activePrimary) < primaryOutputSize()) clearSums();
    }

    primaryAccepted = false;
    if (valid && static_cast<size_t>(activePrimary) < primaryOutputSize()) {
      uint32_t start = 0;
      uint32_t end = 0;
      sourceRange(static_cast<uint16_t>(activePrimary), primarySourceSize(), start, end);
      primaryAccepted = source >= start && source < end;
    }
    secondaryDestination = 0;
  }

  void beginDescendingPrimary(const uint16_t source) {
    if (activePrimary < 0) {
      activePrimary = static_cast<int32_t>(primaryOutputSize()) - 1;
      clearSums();
    }
    while (valid && activePrimary >= 0) {
      uint32_t start = 0;
      uint32_t end = 0;
      sourceRange(static_cast<uint16_t>(activePrimary), primarySourceSize(), start, end);
      if (source >= start) break;
      if (!finalizeActivePrimary()) return;
      --activePrimary;
      if (activePrimary >= 0) clearSums();
    }

    primaryAccepted = false;
    if (valid && activePrimary >= 0) {
      uint32_t start = 0;
      uint32_t end = 0;
      sourceRange(static_cast<uint16_t>(activePrimary), primarySourceSize(), start, end);
      primaryAccepted = source >= start && source < end;
    }
    secondaryDestination = 0;
  }

  void addSecondarySample(const uint16_t source, const uint8_t gray) {
    if (!valid || !primaryAccepted) return;
    while (secondaryDestination < secondaryOutputSize()) {
      uint32_t start = 0;
      uint32_t end = 0;
      sourceRange(secondaryDestination, secondarySourceSize(), start, end);
      if (source < start) return;
      if (source >= end) {
        ++secondaryDestination;
        continue;
      }
      sums[secondaryDestination] += gray;
      primaryHasSamples = true;
      return;
    }
  }

  void consumeRowMajorPixel(const uint16_t x, const uint16_t y, const uint8_t gray) {
    if (!valid || columnMajor || x >= sourceWidth || y >= sourceHeight) return;
    if (lastSourcePrimary != y) {
      if (lastSourcePrimary != std::numeric_limits<uint16_t>::max() && y < lastSourcePrimary) {
        valid = false;
        return;
      }
      lastSourcePrimary = y;
      beginAscendingPrimary(y);
    }
    addSecondarySample(x, gray);
  }

  void consumeColumnMajorPixel(const uint16_t x, const uint16_t y, const uint8_t gray) {
    if (!valid || !columnMajor || x >= sourceWidth || y >= sourceHeight) return;
    if (lastSourcePrimary != x) {
      if (lastSourcePrimary != std::numeric_limits<uint16_t>::max() && x > lastSourcePrimary) {
        valid = false;
        return;
      }
      lastSourcePrimary = x;
      beginDescendingPrimary(x);
    }
    addSecondarySample(y, gray);
  }

  bool finish() {
    if (!valid || finished || lastSourcePrimary == std::numeric_limits<uint16_t>::max()) return false;
    if (activePrimary >= 0 && static_cast<size_t>(activePrimary) < primaryOutputSize() && !finalizeActivePrimary()) {
      return false;
    }
    finished = valid && finalizedPrimary == primaryOutputSize();
    return finished;
  }
};
}  // namespace

class Xtc::ThumbnailPairJob {
  enum class Phase : uint8_t {
    StreamSource,
    FinishThumbnails,
    BeginSharedOutput,
    WriteSharedOutput,
    BeginCarouselOutput,
    WriteCarouselOutput,
    PublishOutputs,
    Done,
    Error,
  };

  std::string sourcePath;
  std::string sharedPath;
  std::string carouselPath;
  std::string outputStagingPath;
  HalFile sourceFile;
  HalFile secondPlaneFile;
  HalFile outputFile;
  xtc::PageLayout pageLayout;
  RawSourceIdentityHandoff sourceIdentityHandoff;
  StreamingThumbnail sharedThumbnail;
  StreamingThumbnail carouselThumbnail;
  const StreamingThumbnail* outputThumbnail = nullptr;
  std::array<uint8_t, MAX_THUMBNAIL_SOURCE_CHUNK> firstChunk{};
  std::array<uint8_t, MAX_THUMBNAIL_SOURCE_CHUNK> secondChunk{};
  uint64_t expectedFileSize = 0;
  size_t sourceBytesRead = 0;
  size_t sourceBytesTotal = 0;
  size_t outputRow = 0;
  uint16_t sourceWidth = 0;
  uint16_t sourceHeight = 0;
  uint8_t bitDepth = 0;
  bool hasSourceIdentityHandoff = false;
  bool sharedReady = false;
  bool carouselReady = false;
  Phase phase = Phase::Error;

  void consumeOneBit(const uint8_t* data, const size_t size, const size_t offset) {
    for (size_t index = 0; index < size; ++index) {
      const size_t sourceOffset = offset + index;
      const uint16_t y = static_cast<uint16_t>(sourceOffset / pageLayout.rowBytes);
      const uint16_t xBase = static_cast<uint16_t>((sourceOffset % pageLayout.rowBytes) * 8U);
      for (uint8_t bit = 0; bit < 8U && xBase + bit < sourceWidth; ++bit) {
        const uint8_t gray = (data[index] & (1U << (7U - bit))) != 0 ? 255U : 0U;
        if (!sharedReady) sharedThumbnail.consumeRowMajorPixel(xBase + bit, y, gray);
        if (!carouselReady) carouselThumbnail.consumeRowMajorPixel(xBase + bit, y, gray);
      }
    }
  }

  void consumeTwoBit(uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t planeOffset) {
    for (size_t index = 0; index < size; ++index) {
      const size_t sourceOffset = planeOffset + index;
      const size_t column = sourceOffset / pageLayout.columnBytes;
      if (column >= sourceWidth) continue;
      const uint16_t x = static_cast<uint16_t>(sourceWidth - 1U - column);
      const uint16_t yBase = static_cast<uint16_t>((sourceOffset % pageLayout.columnBytes) * 8U);
      for (uint8_t bit = 0; bit < 8U && yBase + bit < sourceHeight; ++bit) {
        const uint8_t shift = static_cast<uint8_t>(7U - bit);
        const uint8_t level =
            static_cast<uint8_t>(((bit0[index] >> shift) & 1U) | (((bit1[index] >> shift) & 1U) << 1U));
        const uint8_t gray = static_cast<uint8_t>((3U - level) * 85U);
        if (!sharedReady) sharedThumbnail.consumeColumnMajorPixel(x, yBase + bit, gray);
        if (!carouselReady) carouselThumbnail.consumeColumnMajorPixel(x, yBase + bit, gray);
      }
    }
  }

  bool sourceStillMatches() {
    return sourceFile.isOpen() && sourceFile.fileSize64() == expectedFileSize &&
           (!hasSourceIdentityHandoff || sourceIdentityHandoff.matchesOpenFile(sourcePath, sourceFile)) &&
           (!secondPlaneFile.isOpen() || secondPlaneFile.fileSize64() == expectedFileSize);
  }

  bool closeSource() {
    const bool stable = sourceStillMatches();
    const bool secondClosed = !secondPlaneFile.isOpen() || secondPlaneFile.close();
    const bool sourceClosed = !sourceFile.isOpen() || sourceFile.close();
    return stable && secondClosed && sourceClosed;
  }

  bool beginOutput(const StreamingThumbnail& thumbnail, const std::string& finalPath) {
    if (!thumbnail.finished || !thumbnail.pixels) return false;
    outputThumbnail = &thumbnail;
    outputStagingPath = finalPath + ".tmp";
    outputRow = 0;
    if ((Storage.exists(outputStagingPath.c_str()) && !Storage.remove(outputStagingPath.c_str())) ||
        !Storage.openFileForWrite("XTC", outputStagingPath, outputFile)) {
      return false;
    }
    BmpHeader header;
    createBmpHeader(&header, thumbnail.width, thumbnail.height, BmpRowOrder::TopDown);
    return exactWrite(outputFile, &header, sizeof(header));
  }

  bool writeOutputRows(const size_t maxRows) {
    if (!outputThumbnail || !outputFile.isOpen()) return false;
    const size_t endRow = std::min<size_t>(outputThumbnail->height, outputRow + maxRows);
    while (outputRow < endRow) {
      const uint8_t* row = outputThumbnail->pixels.get() + outputRow * outputThumbnail->rowSize;
      if (!exactWrite(outputFile, row, outputThumbnail->rowSize)) return false;
      ++outputRow;
    }
    return true;
  }

  bool finishOutputStaging() {
    if (!outputThumbnail || outputRow != outputThumbnail->height || !outputFile.isOpen()) return false;
    const bool synced = outputFile.sync();
    const bool closed = outputFile.close();
    outputThumbnail = nullptr;
    return synced && closed;
  }

  bool sourcePathStillMatches() {
    HalFile currentSource;
    if (!Storage.openFileForRead("XTC", sourcePath, currentSource)) return false;
    const bool matches =
        currentSource.fileSize64() == expectedFileSize &&
        (!hasSourceIdentityHandoff || sourceIdentityHandoff.matchesOpenFile(sourcePath, currentSource));
    const bool closed = currentSource.close();
    return matches && closed;
  }

  void cleanup() {
    if (outputFile.isOpen()) outputFile.close();
    if (secondPlaneFile.isOpen()) secondPlaneFile.close();
    if (sourceFile.isOpen()) sourceFile.close();
    if (!outputStagingPath.empty()) Storage.remove(outputStagingPath.c_str());
    if (!sharedPath.empty()) Storage.remove((sharedPath + ".tmp").c_str());
    if (!carouselPath.empty()) Storage.remove((carouselPath + ".tmp").c_str());
  }

  ThumbnailPreparationStatus fail() {
    cleanup();
    phase = Phase::Error;
    return ThumbnailPreparationStatus::Error;
  }

 public:
  ~ThumbnailPairJob() { cleanup(); }

  bool begin(Xtc& book, const int carouselWidth, const int carouselHeight, const bool sharedAlreadyReady,
             const bool carouselAlreadyReady) {
    if (!book.loaded || !book.parser || book.parser->getPageCount() == 0) return false;
    sharedReady = sharedAlreadyReady;
    carouselReady = carouselAlreadyReady;
    sourcePath = book.filepath;
    sharedPath = book.getThumbBmpPath(SHARED_THUMB_HEIGHT);
    carouselPath = book.getThumbBmpPath(carouselHeight);

    xtc::PageInfo pageInfo;
    ZipFile::SourceIdentity sourceIdentity;
    if (!book.parser->getSourceIdentity(sourceIdentity) ||
        !book.parser->openPagePayloadStream(0, pageInfo, sourceFile) || !sourceIdentity.isRawFile()) {
      return false;
    }
    expectedFileSize = sourceIdentity.fileSize;
    sourceWidth = pageInfo.width;
    sourceHeight = pageInfo.height;
    bitDepth = pageInfo.bitDepth;
    if (!xtc::calculatePageLayout(sourceWidth, sourceHeight, bitDepth, pageLayout)) return false;
    if ((!sharedReady && !sharedThumbnail.initialise(sourceWidth, sourceHeight, SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT,
                                                     true, bitDepth == 2)) ||
        (!carouselReady && !carouselThumbnail.initialise(sourceWidth, sourceHeight, carouselWidth, carouselHeight,
                                                         false, bitDepth == 2))) {
      return false;
    }

    if (!book.setupCacheDir()) return false;
    hasSourceIdentityHandoff = book.parser->getSourceIdentityHandoff(sourceIdentityHandoff);
    if (pageInfo.offset > std::numeric_limits<uint64_t>::max() - sizeof(xtc::XtgPageHeader)) return false;
    const uint64_t payloadOffset = pageInfo.offset + sizeof(xtc::XtgPageHeader);
    if (sourceFile.fileSize64() != expectedFileSize ||
        (hasSourceIdentityHandoff && !sourceIdentityHandoff.matchesOpenFile(sourcePath, sourceFile))) {
      return false;
    }

    sourceBytesTotal = bitDepth == 2 ? pageLayout.planeBytes : pageLayout.payloadBytes;
    if (sourceBytesTotal == 0) return false;
    if (bitDepth == 2) {
      if (pageLayout.planeBytes > std::numeric_limits<uint64_t>::max() - payloadOffset ||
          !Storage.openFileForRead("XTC", sourcePath, secondPlaneFile) ||
          secondPlaneFile.fileSize64() != expectedFileSize ||
          !secondPlaneFile.seek64(payloadOffset + pageLayout.planeBytes)) {
        return false;
      }
    } else if (bitDepth != 1) {
      return false;
    }

    phase = Phase::StreamSource;
    return true;
  }

  ThumbnailPreparationStatus step(const size_t maxSourceBytes, const size_t maxOutputRows) {
    if (maxSourceBytes == 0 || maxOutputRows == 0) return fail();

    switch (phase) {
      case Phase::StreamSource: {
        size_t wanted = std::min({maxSourceBytes, firstChunk.size(), sourceBytesTotal - sourceBytesRead});
        if (bitDepth == 2) {
          wanted -= wanted % pageLayout.columnBytes;
          if (wanted == 0) return fail();
        }
        const int firstRead = sourceFile.read(firstChunk.data(), wanted);
        if (firstRead != static_cast<int>(wanted)) return fail();
        if (bitDepth == 1) {
          consumeOneBit(firstChunk.data(), wanted, sourceBytesRead);
        } else {
          const int secondRead = secondPlaneFile.read(secondChunk.data(), wanted);
          if (secondRead != static_cast<int>(wanted)) return fail();
          consumeTwoBit(firstChunk.data(), secondChunk.data(), wanted, sourceBytesRead);
        }
        if ((!sharedReady && !sharedThumbnail.valid) || (!carouselReady && !carouselThumbnail.valid)) return fail();
        sourceBytesRead += wanted;
        if (sourceBytesRead < sourceBytesTotal) return ThumbnailPreparationStatus::InProgress;
        if (!closeSource()) return fail();
        phase = Phase::FinishThumbnails;
        return ThumbnailPreparationStatus::InProgress;
      }
      case Phase::FinishThumbnails:
        if ((!sharedReady && !sharedThumbnail.finish()) || (!carouselReady && !carouselThumbnail.finish())) {
          return fail();
        }
        phase = Phase::BeginSharedOutput;
        return ThumbnailPreparationStatus::InProgress;
      case Phase::BeginSharedOutput:
        if (sharedReady) {
          phase = Phase::BeginCarouselOutput;
        } else {
          if (!beginOutput(sharedThumbnail, sharedPath)) return fail();
          phase = Phase::WriteSharedOutput;
        }
        return ThumbnailPreparationStatus::InProgress;
      case Phase::WriteSharedOutput:
        if (!writeOutputRows(maxOutputRows)) return fail();
        if (outputRow < sharedThumbnail.height) return ThumbnailPreparationStatus::InProgress;
        if (!finishOutputStaging()) return fail();
        phase = Phase::BeginCarouselOutput;
        return ThumbnailPreparationStatus::InProgress;
      case Phase::BeginCarouselOutput:
        if (carouselReady) {
          phase = Phase::PublishOutputs;
        } else {
          if (!beginOutput(carouselThumbnail, carouselPath)) return fail();
          phase = Phase::WriteCarouselOutput;
        }
        return ThumbnailPreparationStatus::InProgress;
      case Phase::WriteCarouselOutput:
        if (!writeOutputRows(maxOutputRows)) return fail();
        if (outputRow < carouselThumbnail.height) return ThumbnailPreparationStatus::InProgress;
        if (!finishOutputStaging()) return fail();
        phase = Phase::PublishOutputs;
        return ThumbnailPreparationStatus::InProgress;
      case Phase::PublishOutputs:
        if (!sourcePathStillMatches()) return fail();
        if ((!sharedReady && !publishBitmap(sharedPath, sharedPath + ".tmp")) ||
            (!carouselReady && !publishBitmap(carouselPath, carouselPath + ".tmp"))) {
          return fail();
        }
        phase = Phase::Done;
        return ThumbnailPreparationStatus::Ready;
      case Phase::Done:
        return ThumbnailPreparationStatus::Ready;
      case Phase::Error:
        return ThumbnailPreparationStatus::Error;
    }
    return fail();
  }
};

Xtc::Xtc(std::string path, const std::string& cacheDir) : filepath(std::move(path)), loaded(false) {
  cachePath = cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(filepath));
}

Xtc::~Xtc() { cancelThumbnailPreparation(); }

bool Xtc::load() {
  if (!beginLoad()) return false;
  while (true) {
    const LoadStepResult result = stepLoad(16, 64U * 1024U);
    if (result == LoadStepResult::Loaded) return true;
    if (result == LoadStepResult::Error) {
      parser.reset();
      return false;
    }
    yield();
  }
}

bool Xtc::beginLoad(const RawSourceIdentityHandoff* const preparedIdentity) {
  if (loaded) return true;
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());
  loaded = false;

  // Initialize parser
  parser = makeUniqueNoThrow<xtc::XtcParser>();
  if (!parser) {
    LOG_ERR("XTC", "Failed to allocate parser");
    return false;
  }

  const xtc::XtcError err = parser->beginOpen(filepath.c_str(), preparedIdentity);
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }
  return true;
}

Xtc::LoadStepResult Xtc::stepLoad(const size_t maxRecords, const size_t maxFingerprintBytes) {
  if (loaded) return LoadStepResult::Loaded;
  if (!parser) return LoadStepResult::Error;
  const xtc::XtcParser::OpenStepResult result = parser->stepOpen(maxRecords, maxFingerprintBytes);
  if (result == xtc::XtcParser::OpenStepResult::InProgress) return LoadStepResult::InProgress;
  if (result == xtc::XtcParser::OpenStepResult::Error) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(parser->getLastError()));
    return LoadStepResult::Error;
  }

  loaded = true;
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return LoadStepResult::Loaded;
}

void Xtc::cancelLoad() {
  if (parser && !loaded) parser->cancelOpen();
  if (!loaded) parser.reset();
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

bool Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return true;
  }

  // HalStorage::mkdir creates missing parents by default. The previous loop
  // retried recursive mkdir for every path component before doing it again for
  // the final path.
  if (!Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to create cache directory: %s", cachePath.c_str());
    return false;
  }
  return true;
}

bool Xtc::readCoreMetadata(std::string& title, std::string& author) const {
  return xtc::XtcParser::readCoreMetadata(filepath.c_str(), title, author) == xtc::XtcError::OK;
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
  if (!setupCacheDir()) return false;

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

Xtc::ThumbnailPreparationStatus Xtc::beginThumbnailPreparation(const int carouselWidth, const int carouselHeight) {
  cancelThumbnailPreparation();
  if (carouselWidth <= 0 || carouselHeight <= 0) return ThumbnailPreparationStatus::Error;

  const std::string sharedPath = getThumbBmpPath(SHARED_THUMB_HEIGHT);
  const std::string carouselPath = getThumbBmpPath(carouselHeight);
  bool sharedIoError = false;
  bool carouselIoError = false;
  const bool sharedReady =
      thumbnailCacheReady(sharedPath, SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT, true, sharedIoError);
  const bool carouselReady = thumbnailCacheReady(carouselPath, carouselWidth, carouselHeight, false, carouselIoError);
  if (sharedIoError || carouselIoError) return ThumbnailPreparationStatus::Error;
  if (sharedReady && carouselReady) return ThumbnailPreparationStatus::Ready;
  if (!loaded || !parser || parser->getPageCount() == 0) return ThumbnailPreparationStatus::NeedsSource;

  thumbnailPairJob = makeUniqueNoThrow<ThumbnailPairJob>();
  if (!thumbnailPairJob || !thumbnailPairJob->begin(*this, carouselWidth, carouselHeight, sharedReady, carouselReady)) {
    thumbnailPairJob.reset();
    return ThumbnailPreparationStatus::Error;
  }
  return ThumbnailPreparationStatus::InProgress;
}

Xtc::ThumbnailPreparationStatus Xtc::stepThumbnailPreparation(const size_t maxSourceBytes, const size_t maxOutputRows) {
  if (!thumbnailPairJob) return ThumbnailPreparationStatus::Error;
  const ThumbnailPreparationStatus status = thumbnailPairJob->step(maxSourceBytes, maxOutputRows);
  if (status != ThumbnailPreparationStatus::InProgress) thumbnailPairJob.reset();
  return status;
}

void Xtc::cancelThumbnailPreparation() { thumbnailPairJob.reset(); }

bool Xtc::generateThumbBmpPair(const int carouselWidth, const int carouselHeight) {
  ThumbnailPreparationStatus status = beginThumbnailPreparation(carouselWidth, carouselHeight);
  while (status == ThumbnailPreparationStatus::InProgress) {
    status = stepThumbnailPreparation();
    yield();
  }
  return status == ThumbnailPreparationStatus::Ready;
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
  if (!setupCacheDir()) return false;

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

bool Xtc::getSourceIdentityHandoff(RawSourceIdentityHandoff& handoff) const {
  return loaded && parser && parser->getSourceIdentityHandoff(handoff);
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

xtc::XtcError Xtc::loadXthPlanePairs(
    uint32_t pageIndex, std::function<void(uint8_t* bit0, uint8_t* bit1, size_t size, size_t planeOffset)> callback,
    size_t chunkSize) const {
  if (!loaded || !parser) return xtc::XtcError::FILE_NOT_FOUND;
  return const_cast<xtc::XtcParser*>(parser.get())->loadXthPlanePairs(pageIndex, callback, chunkSize);
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
