#include "Txt.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <RawSourceIdentity.h>
#include <StagedFileTransaction.h>

#include <algorithm>
#include <array>
#include <limits>

namespace {
constexpr size_t IDENTITY_CHUNK_SIZE = 2048;
constexpr size_t IDENTITY_YIELD_BYTES = 64U * 1024U;
}  // namespace

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

bool Txt::load() {
  if (!beginLoad()) return false;
  while (true) {
    const LoadStepResult result = stepLoad(IDENTITY_YIELD_BYTES);
    if (result == LoadStepResult::Loaded) return true;
    if (result == LoadStepResult::Error) return false;
    yield();
  }
}

bool Txt::beginLoad(const RawSourceIdentityHandoff* const preparedIdentity) {
  if (loaded) return true;
  cancelLoad();

  if (!Storage.openFileForRead("TXT", filepath, loadFile)) {
    LOG_ERR("TXT", "File does not exist or cannot be opened: %s", filepath.c_str());
    return false;
  }

  loadExpectedSize = loadFile.fileSize64();
  if (loadExpectedSize > std::numeric_limits<size_t>::max() ||
      loadExpectedSize > std::numeric_limits<uint32_t>::max()) {
    LOG_ERR("TXT", "TXT file is too large for this build: %s", filepath.c_str());
    loadFile.close();
    loadExpectedSize = 0;
    return false;
  }

  if (preparedIdentity && preparedIdentity->matchesOpenFile(filepath, loadFile)) {
    const bool closed = loadFile.close();
    if (!closed) {
      cancelLoad();
      return false;
    }
    fileSize = static_cast<size_t>(loadExpectedSize);
    sourceIdentity = preparedIdentity->identity;
    sourceIdentityHandoff = *preparedIdentity;
    loadExpectedSize = 0;
    loaded = true;
    LOG_DBG("TXTM", "identity_reused bytes=%zu", fileSize);
    return true;
  }

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  loadStartedMs = static_cast<uint32_t>(millis());
  loadStartFreeHeap = ESP.getFreeHeap();
#endif

  loadBytesRead = 0;
  loadFingerprint = {};
  loadInProgress = true;
  return true;
}

Txt::LoadStepResult Txt::stepLoad(const size_t maxBytes) {
  if (loaded) return LoadStepResult::Loaded;
  if (!loadInProgress || maxBytes == 0) return LoadStepResult::Error;

  std::array<uint8_t, IDENTITY_CHUNK_SIZE> buffer;
  size_t bytesThisStep = 0;
  while (loadBytesRead < loadExpectedSize && bytesThisStep < maxBytes) {
    const size_t wanted = static_cast<size_t>(
        std::min<uint64_t>({buffer.size(), loadExpectedSize - loadBytesRead, maxBytes - bytesThisStep}));
    const int bytesRead = loadFile.read(buffer.data(), wanted);
    if (bytesRead <= 0 || static_cast<size_t>(bytesRead) > wanted) {
      LOG_ERR("TXT", "Failed to fingerprint complete file: %s", filepath.c_str());
      cancelLoad();
      return LoadStepResult::Error;
    }
    loadFingerprint.update(buffer.data(), static_cast<size_t>(bytesRead));
    loadBytesRead += static_cast<size_t>(bytesRead);
    bytesThisStep += static_cast<size_t>(bytesRead);
  }
  if (loadBytesRead < loadExpectedSize) return LoadStepResult::InProgress;

  // Reject an append/truncation observed during the scan. A writer must not
  // bind path-keyed progress or statistics to a prefix of a changing file.
  const bool sizeStayedStable = loadFile.fileSize64() == loadExpectedSize;
  const ZipFile::SourceIdentity completedIdentity = loadFingerprint.finish(loadExpectedSize);
  RawSourceIdentityHandoff completedHandoff;
  const bool handoffCaptured = sizeStayedStable && completedHandoff.capture(filepath, loadFile, completedIdentity);
  const bool closed = loadFile.close();
  if (loadBytesRead != loadExpectedSize || !sizeStayedStable || !closed) {
    LOG_ERR("TXT", "TXT changed or became unreadable while fingerprinting: %s", filepath.c_str());
    cancelLoad();
    return LoadStepResult::Error;
  }

  fileSize = static_cast<size_t>(loadExpectedSize);
  sourceIdentity = completedIdentity;
  sourceIdentityHandoff = handoffCaptured ? std::move(completedHandoff) : RawSourceIdentityHandoff{};

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("TXTM", "identity bytes=%llu elapsed_ms=%u heap_delta=%ld free_heap=%u max_alloc=%u min_free_heap=%u",
          static_cast<unsigned long long>(loadExpectedSize),
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - loadStartedMs),
          static_cast<long>(static_cast<int32_t>(loadStartFreeHeap) - static_cast<int32_t>(freeHeap)),
          static_cast<unsigned>(freeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
#endif

  loadInProgress = false;
  loaded = true;
  LOG_DBG("TXT", "Loaded TXT file: %s (%zu bytes)", filepath.c_str(), fileSize);
  return LoadStepResult::Loaded;
}

void Txt::cancelLoad() {
  if (loadFile.isOpen()) loadFile.close();
  loadInProgress = false;
  loadExpectedSize = 0;
  loadBytesRead = 0;
  loadFingerprint = {};
  if (!loaded) sourceIdentityHandoff = {};
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove the plain-text extension used to dispatch this reader.
  if (FsHelpers::hasTxtExtension(filename)) {
    filename.resize(filename.length() - 4);
  } else if (FsHelpers::hasMarkdownExtension(filename)) {
    filename.resize(filename.length() - 3);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".BMP", ".JPG", ".JPEG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      LOG_DBG("TXT", "Found matching cover image: %s", coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        LOG_DBG("TXT", "Found fallback cover image: %s", coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  const std::string finalPath = getCoverBmpPath();
  const std::string stagingPath = finalPath + ".tmp";
  const std::string backupPath = finalPath + ".bak";
  const BitmapCacheState cacheState = Bitmap::inspectDerivedCache(finalPath);
  if (cacheState == BitmapCacheState::Ready) return true;
  if (cacheState == BitmapCacheState::IoError) return false;

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG("TXT", "No cover image found for TXT file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;

  const bool bmpSource = FsHelpers::hasBmpExtension(coverImagePath);
  const bool jpegSource = FsHelpers::hasJpgExtension(coverImagePath);
  if (!bmpSource && !jpegSource) {
    // PNG files are not supported by this lightweight sidecar-cover path.
    LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG)");
    return false;
  }

  HalFile source;
  if (!Storage.openFileForRead("TXT", coverImagePath, source)) return false;
  HalFile output;
  if (!Storage.openFileForWrite("TXT", stagingPath, output)) {
    source.close();
    return false;
  }

  bool converted = true;
  if (bmpSource) {
    // Copy BMP file to cache
    LOG_DBG("TXT", "Copying BMP cover image to cache");
    uint8_t buffer[1024];
    while (source.available()) {
      const int bytesRead = source.read(buffer, sizeof(buffer));
      if (bytesRead <= 0 || output.write(buffer, static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
        converted = false;
        break;
      }
    }
  } else {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    LOG_DBG("TXT", "Generating BMP from JPG cover image");
    converted = JpegToBmpConverter::jpegFileToBmpStream(source, output);
  }

  const bool inputOk = source.getError() == 0;
  const bool inputClosed = source.close();
  const bool outputSynced = output.sync();
  const bool outputClosed = output.close();
  const auto published = converted && inputOk && inputClosed && outputSynced && outputClosed
                             ? StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(),
                                                              backupPath.c_str(), Bitmap::validateFile)
                             : StagedFileTransaction::Status::IoError;
  if (published != StagedFileTransaction::Status::Published) {
    Storage.remove(stagingPath.c_str());
    LOG_ERR("TXT", "Failed to publish BMP cover cache");
    return false;
  }

  LOG_DBG("TXT", "Published BMP cover cache: %s", finalPath.c_str());
  return true;
}

bool Txt::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("TXT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("TXT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("TXT", "Cache cleared successfully");
  return true;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    return false;
  }

  return readContent(file, buffer, offset, length);
}

bool Txt::readContent(HalFile& file, uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded || !file || (!buffer && length != 0)) return false;

  if (!file.seek(offset)) {
    return false;
  }

  const int bytesRead = file.read(buffer, length);
  return bytesRead >= 0 && static_cast<size_t>(bytesRead) == length;
}
