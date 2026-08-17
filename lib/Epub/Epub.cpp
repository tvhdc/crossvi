#include "Epub.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <StagedFileTransaction.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <new>
#include <string_view>

#include "Epub/SourceIdentityCodec.h"
#include "Epub/SourceIdentityStore.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
constexpr char NO_COVER_MAGIC[] = "CVNC1";
constexpr size_t GUIDE_COVER_PAGE_MAX_BYTES = 32U * 1024U;
constexpr size_t MAX_EXTRACTED_COVER_BYTES = 16U * 1024U * 1024U;
constexpr uint16_t ZIP_METHOD_STORED_LOCAL = 0;

struct ThumbnailValidationContext {
  int width = 0;
  int height = 0;
  uint64_t fileSize = 0;
  bool fitWithin = false;
};

constexpr uint64_t thumbnailFileSize(const int width, const int height) {
  return 62U + ((static_cast<uint64_t>(width) + 31U) / 32U * 4U) * static_cast<uint64_t>(height);
}

struct ThumbIdentityContext {
  const ZipFile::SourceIdentity* expected = nullptr;
};

bool readThumbIdentity(const char* path, ZipFile::SourceIdentity& identity) {
  if (!path) return false;
  HalFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return false;
  SourceIdentityCodec::Encoded encoded{};
  const bool read = file.fileSize64() == encoded.size() &&
                    file.read(encoded.data(), encoded.size()) == static_cast<int>(encoded.size());
  const bool closed = file.close();
  return read && closed &&
         SourceIdentityCodec::decode(encoded.data(), encoded.size(), identity) == SourceIdentityCodec::DecodeStatus::OK;
}

bool validateThumbIdentity(const char* path, void* rawContext) {
  const auto* context = static_cast<const ThumbIdentityContext*>(rawContext);
  if (!context || !context->expected) return false;
  ZipFile::SourceIdentity actual;
  return readThumbIdentity(path, actual) && actual == *context->expected;
}

bool publishThumbIdentity(const std::string& path, const ZipFile::SourceIdentity& identity) {
  SourceIdentityCodec::Encoded encoded{};
  if (!SourceIdentityCodec::encode(identity, encoded)) return false;
  const std::string staging = path + ".tmp";
  const std::string backup = path + ".bak";
  ThumbIdentityContext context{&identity};
  if (StagedFileTransaction::recover(path.c_str(), backup.c_str(), validateThumbIdentity, &context) ==
          StagedFileTransaction::Status::IoError ||
      (Storage.exists(staging.c_str()) && !Storage.remove(staging.c_str()))) {
    return false;
  }
  HalFile file;
  if (!Storage.openFileForWrite("EBP", staging, file)) return false;
  const bool written = file.write(encoded.data(), encoded.size()) == static_cast<int>(encoded.size());
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!written || !synced || !closed) {
    Storage.remove(staging.c_str());
    return false;
  }
  return StagedFileTransaction::publish(path.c_str(), staging.c_str(), backup.c_str(), validateThumbIdentity,
                                        &context) == StagedFileTransaction::Status::Published;
}

bool validateThumbnailBitmap(const char* path, void* rawContext, const bool exactDimensions) {
  const auto* context = static_cast<const ThumbnailValidationContext*>(rawContext);
  if (!path || !context || context->width <= 0 || context->height <= 0) return false;
  HalFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return false;
  if (exactDimensions && file.fileSize64() != context->fileSize) {
    file.close();
    return false;
  }
  Bitmap bitmap(file);
  const bool parsed = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getBpp() == 1;
  const uint64_t packedBytes = parsed ? static_cast<uint64_t>(bitmap.getRowBytes()) * bitmap.getHeight() : 0;
  bool dimensionsValid = false;
  if (exactDimensions) {
    dimensionsValid = parsed && bitmap.getWidth() == context->width && bitmap.getHeight() == context->height;
  } else if (context->fitWithin) {
    dimensionsValid = parsed && bitmap.getWidth() > 0 && bitmap.getHeight() > 0 &&
                      bitmap.getWidth() <= context->width && bitmap.getHeight() <= context->height &&
                      (bitmap.getWidth() >= context->width - 1 || bitmap.getHeight() >= context->height - 1) &&
                      packedBytes > 0 && packedBytes <= 64U * 1024U;
  } else {
    dimensionsValid = parsed && bitmap.getWidth() >= context->width - 1 && bitmap.getHeight() >= context->height - 1 &&
                      packedBytes > 0 && packedBytes <= 64U * 1024U;
  }
  const bool closed = file.close();
  return dimensionsValid && closed;
}

bool validateCachedThumbnail(const char* path, void* context) { return validateThumbnailBitmap(path, context, false); }

enum class MarkerFileStatus : uint8_t { Missing, Valid, Invalid, IoError };

MarkerFileStatus inspectNoCoverMarker(const char* path) {
  if (!path || !Storage.exists(path)) return MarkerFileStatus::Missing;
  HalFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return MarkerFileStatus::IoError;
  if (file.fileSize64() != sizeof(NO_COVER_MAGIC)) {
    return file.close() ? MarkerFileStatus::Invalid : MarkerFileStatus::IoError;
  }
  char magic[sizeof(NO_COVER_MAGIC)]{};
  const bool valid = file.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
                     std::memcmp(magic, NO_COVER_MAGIC, sizeof(magic)) == 0;
  const bool ioError = file.getError() != 0;
  const bool closed = file.close();
  if (ioError || !closed) return MarkerFileStatus::IoError;
  return valid ? MarkerFileStatus::Valid : MarkerFileStatus::Invalid;
}

bool validateNoCoverMarker(const char* path, void*) { return inspectNoCoverMarker(path) == MarkerFileStatus::Valid; }

bool writeNoCoverMarker(const std::string& finalPath) {
  const std::string stagingPath = finalPath + ".tmp";
  const std::string backupPath = finalPath + ".bak";
  const MarkerFileStatus finalStatus = inspectNoCoverMarker(finalPath.c_str());
  if (finalStatus == MarkerFileStatus::IoError) return false;
  if (finalStatus == MarkerFileStatus::Valid) {
    StagedFileTransaction::recover(finalPath.c_str(), backupPath.c_str(), validateNoCoverMarker);
    return true;
  }
  const MarkerFileStatus backupStatus = inspectNoCoverMarker(backupPath.c_str());
  if (backupStatus == MarkerFileStatus::IoError) return false;
  if (backupStatus == MarkerFileStatus::Valid &&
      StagedFileTransaction::recover(finalPath.c_str(), backupPath.c_str(), validateNoCoverMarker) ==
          StagedFileTransaction::Status::IoError) {
    return false;
  }
  if (backupStatus == MarkerFileStatus::Invalid && !Storage.remove(backupPath.c_str())) return false;

  if (inspectNoCoverMarker(finalPath.c_str()) == MarkerFileStatus::Valid) return true;
  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;
  HalFile marker;
  if (!Storage.openFileForWrite("EBP", stagingPath, marker)) return false;
  const bool written = marker.write(NO_COVER_MAGIC, sizeof(NO_COVER_MAGIC)) == sizeof(NO_COVER_MAGIC);
  const bool synced = marker.sync();
  const bool closed = marker.close();
  if (!written || !synced || !closed) {
    Storage.remove(stagingPath.c_str());
    return false;
  }
  return StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                        validateNoCoverMarker) == StagedFileTransaction::Status::Published;
}

bool publishBitmap(const std::string& finalPath, const std::string& stagingPath) {
  const std::string backupPath = finalPath + ".bak";
  return StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                        Bitmap::validateFile, nullptr) == StagedFileTransaction::Status::Published;
}

bool validateRasterFile(const char* path, void*) {
  if (!path) return false;
  HalFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return false;
  std::string_view imagePath(path);
  if (imagePath.size() >= 4 &&
      (imagePath.substr(imagePath.size() - 4) == ".tmp" || imagePath.substr(imagePath.size() - 4) == ".bak")) {
    imagePath.remove_suffix(4);
  }
  if (!FsHelpers::hasJpgExtension(imagePath) && !FsHelpers::hasPngExtension(imagePath)) {
    file.close();
    return false;
  }

  ImageDimsProbe probe;
  std::array<uint8_t, 1024> buffer;
  bool readOk = true;
  while (file.available()) {
    const size_t bytesRead = file.read(buffer.data(), buffer.size());
    if (bytesRead == 0) {
      readOk = false;
      break;
    }
    const size_t consumed = probe.write(buffer.data(), bytesRead);
    if (consumed != bytesRead) break;
  }
  const bool closed = file.close();
  ImageDimensions dimensions{};
  return readOk && closed && probe.getDimensions(dimensions);
}

constexpr std::array<uint8_t, 5> IMAGE_PUBLISH_MARKER{'C', 'V', 'I', 'P', 1};

bool writeImagePublishMarker(const std::string& markerPath) {
  if (Storage.exists(markerPath.c_str()) && !Storage.remove(markerPath.c_str())) return false;
  HalFile marker;
  if (!Storage.openFileForWrite("EBP", markerPath, marker)) return false;
  const bool written =
      marker.write(IMAGE_PUBLISH_MARKER.data(), IMAGE_PUBLISH_MARKER.size()) == IMAGE_PUBLISH_MARKER.size();
  const bool synced = written && marker.sync();
  const bool closed = marker.close();
  if (!written || !synced || !closed) Storage.remove(markerPath.c_str());
  return written && synced && closed;
}

bool reconcileImagePublication(const std::string& finalPath, const std::string& stagingPath,
                               const std::string& backupPath, const std::string& markerPath) {
  if (!Storage.exists(markerPath.c_str())) {
    return StagedFileTransaction::recover(finalPath.c_str(), backupPath.c_str(), validateRasterFile) !=
           StagedFileTransaction::Status::IoError;
  }

  if (Storage.exists(backupPath.c_str())) {
    // A retained backup proves publication began but was never committed.
    // Restore it even when the new final happens to have a valid image header.
    if (!StagedFileTransaction::rollbackPendingPublish(finalPath.c_str(), backupPath.c_str())) return false;
  } else {
    // A marker without a backup may precede publication or may refer to an
    // unverified new final. Both files are derived data, so fail closed and
    // regenerate instead of trying to infer which rename completed.
    if (Storage.exists(finalPath.c_str()) && !Storage.remove(finalPath.c_str())) return false;
  }

  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;
  return Storage.remove(markerPath.c_str());
}

bool sourcePathMatchesIdentityJob(const char* path, const ZipFile::SourceIdentity& identity,
                                  const ZipSourceIdentityJob::FileStamp& stamp) {
  if (!path || !stamp.valid) return false;
  HalFile current;
  if (!Storage.openFileForRead("EBP", path, current)) return false;
  uint16_t modifyDate = 0;
  uint16_t modifyTime = 0;
  const bool sizeMatches = current.fileSize64() == identity.fileSize;
  const bool stampMatches = current.getModifyDateTime(&modifyDate, &modifyTime) && modifyDate == stamp.modifyDate &&
                            modifyTime == stamp.modifyTime;
  return current.close() && sizeMatches && stampMatches;
}
}  // namespace

class Epub::IndexingReadState {
 public:
  enum class Kind : uint8_t { Container, Opf, TocNav, TocNcx };

  Kind kind = Kind::Container;
  std::string entryPath;
  std::string basePath;
  ZipStreamReadJob job;
  std::unique_ptr<ContainerParser> containerParser;
  std::unique_ptr<ContentOpfParser> opfParser;
  std::unique_ptr<TocNavParser> navParser;
  std::unique_ptr<TocNcxParser> ncxParser;
};

class Epub::CoreMetadataReadState final : public Print {
 public:
  enum class Phase : uint8_t { Source, Cache, Container, Opf, Guide, Verify };

  size_t write(const uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* buffer, const size_t size) override {
    if (!buffer || !guideBuffer || guideWritten > guideSize || size > guideSize - guideWritten) return 0;
    memcpy(guideBuffer.get() + guideWritten, buffer, size);
    guideWritten += size;
    return size;
  }

  Phase phase = Phase::Source;
  ZipSourceIdentityJob identityJob;
  ZipStreamReadJob streamJob;
  ZipFile::SourceIdentity initialIdentity{};
  BookMetadataCache::BookMetadata metadata;
  std::string entryPath;
  std::string guidePath;
  std::unique_ptr<ContainerParser> containerParser;
  std::unique_ptr<ContentOpfParser> opfParser;
  std::unique_ptr<uint8_t[]> guideBuffer;
  size_t guideSize = 0;
  size_t guideWritten = 0;
  bool cacheLoadActive = false;
  bool verifyStarted = false;
};

class Epub::ImageDigestingOutput final : public Print {
 public:
  explicit ImageDigestingOutput(HalFile& output) : output_(output) {}

  size_t write(const uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, const size_t size) override {
    if (!data || size == 0) return 0;
    const size_t written = output_.write(data, size);
    StagedFileTransaction::updateDigest(digest_, data, written);
    if (!probeSettled_) {
      const size_t consumed = probe_.write(data, written);
      ImageDimensions dimensions{};
      if (probe_.getDimensions(dimensions)) {
        probeSettled_ = true;
      } else if (consumed != written) {
        probeSettled_ = true;
        probeFailed_ = true;
      }
    }
    return written;
  }

  const StagedFileTransaction::Digest& digest() const { return digest_; }
  bool validRaster() const {
    ImageDimensions dimensions{};
    return !probeFailed_ && probe_.getDimensions(dimensions);
  }

 private:
  HalFile& output_;
  StagedFileTransaction::Digest digest_;
  ImageDimsProbe probe_;
  bool probeSettled_ = false;
  bool probeFailed_ = false;
};

class Epub::ImageDigestReadJob {
 public:
  enum class StepStatus : uint8_t { InProgress, Done, Error };

  ~ImageDigestReadJob() { cancel(); }

  bool begin(const std::string& path) {
    cancel();
    if (!Storage.openFileForRead("EBP", path, input_)) return false;
    expectedSize_ = input_.fileSize64();
    remaining_ = expectedSize_;
    digest_ = {};
    return expectedSize_ > 0;
  }

  StepStatus step(const size_t maxBytes) {
    if (!input_ || maxBytes == 0) return StepStatus::Error;
    std::array<uint8_t, 512> buffer;
    size_t budget = std::min<uint64_t>(maxBytes, remaining_);
    while (budget > 0) {
      const size_t chunk = std::min({buffer.size(), budget, static_cast<size_t>(remaining_)});
      if (input_.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
        cancel();
        return StepStatus::Error;
      }
      StagedFileTransaction::updateDigest(digest_, buffer.data(), chunk);
      remaining_ -= chunk;
      budget -= chunk;
    }
    if (remaining_ > 0) return StepStatus::InProgress;
    const bool sizeUnchanged = input_.fileSize64() == expectedSize_;
    const bool closed = input_.close();
    return sizeUnchanged && closed && digest_.size == expectedSize_ ? StepStatus::Done : StepStatus::Error;
  }

  const StagedFileTransaction::Digest& digest() const { return digest_; }

  void cancel() {
    if (input_) input_.close();
    input_ = {};
    expectedSize_ = 0;
    remaining_ = 0;
  }

 private:
  HalFile input_;
  StagedFileTransaction::Digest digest_;
  uint64_t expectedSize_ = 0;
  uint64_t remaining_ = 0;
};

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
}

Epub::Epub(std::string filepath, const std::string& cacheDir, const ZipFile::SourceIdentity& verifiedSourceIdentity)
    : Epub(std::move(filepath), cacheDir) {
  sourceIdentitySnapshot = verifiedSourceIdentity;
  hasSourceIdentitySnapshot = true;
  sourceReplacementRecoveryDone = true;
}

Epub::~Epub() {
  cancelCoreMetadataRead();
  cancelIndexing();
  cancelThumbnailPreparation();
  cancelImagePreparation();
  clearCoverSource();
}

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
namespace {
class EpubLoadDebugMetric {
 public:
  EpubLoadDebugMetric() : startedMs_(static_cast<uint32_t>(millis())), startFreeHeap_(ESP.getFreeHeap()) {}

  ~EpubLoadDebugMetric() {
    const uint32_t freeHeap = ESP.getFreeHeap();
    const int32_t heapDelta = static_cast<int32_t>(startFreeHeap_) - static_cast<int32_t>(freeHeap);
    LOG_DBG("IDX",
            "EPUB metadata load: cache=%s status=%u result=%s elapsed_ms=%u heap_delta=%ld free_heap=%u "
            "min_free_heap=%u",
            cache_, cacheStatus_, success_ ? "ok" : "failed",
            static_cast<unsigned>(static_cast<uint32_t>(millis()) - startedMs_), static_cast<long>(heapDelta),
            static_cast<unsigned>(freeHeap), static_cast<unsigned>(ESP.getMinFreeHeap()));
  }

  void classifyCache(const BookMetadataCache::LoadStatus status, const bool buildIfMissing) {
    cacheStatus_ = static_cast<unsigned>(status);
    if (status == BookMetadataCache::LoadStatus::Loaded) {
      cache_ = "hit";
    } else if (buildIfMissing && status != BookMetadataCache::LoadStatus::NewerVersion &&
               status != BookMetadataCache::LoadStatus::IoError) {
      cache_ = "rebuild";
    } else {
      cache_ = "miss";
    }
  }

  void markSuccess() { success_ = true; }

 private:
  uint32_t startedMs_ = 0;
  uint32_t startFreeHeap_ = 0;
  unsigned cacheStatus_ = UINT8_MAX;
  const char* cache_ = "uninspected";
  bool success_ = false;
};
}  // namespace
#endif

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  coverResolutionComplete = false;
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }
  if (!opfParser.succeeded()) {
    LOG_ERR("EBP", "content.opf parser did not complete safely");
    return false;
  }

  return finalizeContentOpf(opfParser, bookMetadata);
}

bool Epub::finalizeContentOpf(ContentOpfParser& opfParser, BookMetadataCache::BookMetadata& bookMetadata,
                              const bool resolveGuide) {
  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  coverResolutionComplete = !bookMetadata.coverItemHref.empty() || opfParser.guideCoverPageHref.empty();

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (resolveGuide && bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize = 0;
    uint8_t* coverPageData = nullptr;
    if (!getItemSize(opfParser.guideCoverPageHref, &coverPageSize)) {
      LOG_ERR("EBP", "Could not size guide cover page");
    } else if (coverPageSize > GUIDE_COVER_PAGE_MAX_BYTES) {
      LOG_ERR("EBP", "Guide cover page is too large (%zu bytes)", coverPageSize);
    } else {
      coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    }
    if (coverPageData) {
      resolveGuideCover(opfParser.guideCoverPageHref, coverPageData, coverPageSize, bookMetadata);
      free(coverPageData);
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

void Epub::resolveGuideCover(const std::string& guidePath, const uint8_t* contents, const size_t size,
                             BookMetadataCache::BookMetadata& bookMetadata) {
  if (!contents && size != 0) return;
  const std::string_view coverPageHtml(reinterpret_cast<const char*>(contents), size);

  std::string coverPageBase;
  const auto lastSlash = guidePath.rfind('/');
  if (lastSlash != std::string::npos) coverPageBase = guidePath.substr(0, lastSlash + 1);

  std::string imageRef;
  for (const char* pattern : {"xlink:href=\"", "xlink:href='", "src=\"", "src='"}) {
    auto pos = coverPageHtml.find(pattern);
    while (pos != std::string_view::npos) {
      const size_t patternLength = strlen(pattern);
      const char quote = pattern[patternLength - 1];
      pos += patternLength;
      const auto endPos = coverPageHtml.find(quote, pos);
      if (endPos != std::string_view::npos) {
        const auto ref = coverPageHtml.substr(pos, endPos - pos);
        // Cover BMP generation supports JPG/PNG only; skip GIF so an
        // unsupported wrapper image does not block a later supported one.
        if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
          imageRef = ref;
          break;
        }
      }
      pos = coverPageHtml.find(pattern, pos);
    }
    if (!imageRef.empty()) break;
  }

  // A complete guide wrapper with no supported image is an authoritative
  // no-cover result. Failed or oversized reads deliberately never call here.
  coverResolutionComplete = true;
  if (!imageRef.empty()) {
    bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
    LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
  }
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);
  cssDiscoveryComplete = true;

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
    cssDiscoveryComplete = false;
  }
}

bool Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return true;
  }

  if (!cssDiscoveryComplete) {
    LOG_ERR("EBP", "Refusing to publish a partial CSS cache after ZIP enumeration failed");
    cssParser->clear();
    return false;
  }

  // No cache yet - parse CSS files
  bool complete = true;
  const std::string tmpCssPath = getCachePath() + "/.tmp.css";
  for (const auto& cssPath : cssFiles) {
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      complete = false;
      break;
    }

    // Check CSS file size before decompressing. A listed stylesheet that cannot
    // be read safely makes the whole external-style cache incomplete.
    size_t cssFileSize = 0;
    if (!getItemSize(cssPath, &cssFileSize)) {
      LOG_ERR("EBP", "Could not inspect CSS file: %s", cssPath.c_str());
      complete = false;
      break;
    }
    if (cssFileSize > MAX_CSS_FILE_SIZE) {
      LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max): %s", cssFileSize, MAX_CSS_FILE_SIZE, cssPath.c_str());
      complete = false;
      break;
    }

    // Extract CSS file to temp location
    HalFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      complete = false;
      break;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      complete = false;
      break;
    }
    // Explicitly close() file before reopening for reading
    if (!tempCssFile.close()) {
      LOG_ERR("EBP", "Could not close extracted CSS file: %s", cssPath.c_str());
      Storage.remove(tmpCssPath.c_str());
      complete = false;
      break;
    }

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      complete = false;
      break;
    }
    if (tempCssFile.fileSize() != cssFileSize || !cssParser->loadFromStream(tempCssFile)) {
      LOG_ERR("EBP", "Extracted CSS file was incomplete: %s", cssPath.c_str());
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      complete = false;
      break;
    }
    // Explicitly close() file before calling Storage.remove()
    const bool closed = tempCssFile.close();
    const bool removed = Storage.remove(tmpCssPath.c_str());
    if (!closed || !removed) {
      LOG_ERR("EBP", "Could not finalize extracted CSS file: %s", cssPath.c_str());
      complete = false;
      break;
    }
  }

  if (!complete) {
    if (Storage.exists(tmpCssPath.c_str())) Storage.remove(tmpCssPath.c_str());
    cssParser->clear();
    cssParser->deleteCache();
    return false;
  }

  // Save to cache for next time
  const bool saved = cssParser->saveToCache();
  if (!saved) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files", cssParser->ruleCount(), cssFiles.size());
  cssParser->clear();
  return saved;
}

bool Epub::ensureSourceIdentitySnapshot() const {
  if (hasSourceIdentitySnapshot) return true;
  ZipFile currentFile(filepath);
  if (!currentFile.getSourceIdentity(sourceIdentitySnapshot)) return false;
  hasSourceIdentitySnapshot = true;
  return true;
}

bool Epub::sourceStillMatchesSnapshot() const {
  if (!hasSourceIdentitySnapshot) return false;
  ZipFile currentFile(filepath);
  ZipFile::SourceIdentity current;
  return currentFile.getSourceIdentity(current) && current == sourceIdentitySnapshot;
}

Epub::SourceBindingStatus Epub::inspectSourceBinding() const {
  if (!ensureSourceIdentitySnapshot()) return SourceBindingStatus::IoError;

  // A power loss after the durable replacement marker was published but
  // before the backing file changed leaves the old EPUB authoritative. Restore
  // its retained identity instead of misclassifying it as a replacement.
  if (sourceReplacementRecoveryDone) {
    sourceReplacementRecoveryDone = false;
  } else {
    switch (SourceIdentityStore::recoverReplacement(cachePath, sourceIdentitySnapshot)) {
      case SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource:
      case SourceIdentityStore::RecoverReplacementStatus::NotPrepared:
      case SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished:
        break;
      case SourceIdentityStore::RecoverReplacementStatus::NewerVersion:
        return SourceBindingStatus::NewerVersion;
      case SourceIdentityStore::RecoverReplacementStatus::Invalid:
        return SourceBindingStatus::Invalid;
      case SourceIdentityStore::RecoverReplacementStatus::IoError:
        return SourceBindingStatus::IoError;
    }
  }

  ZipFile::SourceIdentity stored;
  switch (SourceIdentityStore::load(cachePath, stored)) {
    case SourceIdentityStore::LoadStatus::Primary:
    case SourceIdentityStore::LoadStatus::Backup:
    case SourceIdentityStore::LoadStatus::Temp:
      return stored == sourceIdentitySnapshot ? SourceBindingStatus::Match : SourceBindingStatus::Mismatch;
    case SourceIdentityStore::LoadStatus::Missing:
      return SourceBindingStatus::Missing;
    case SourceIdentityStore::LoadStatus::NewerVersion:
      return SourceBindingStatus::NewerVersion;
    case SourceIdentityStore::LoadStatus::Invalid:
      return SourceBindingStatus::Invalid;
    case SourceIdentityStore::LoadStatus::IoError:
      return SourceBindingStatus::IoError;
  }
  return SourceBindingStatus::IoError;
}

Epub::SourceBindingStatus Epub::inspectSourceBindingForLoad() {
  const SourceBindingStatus status = inspectSourceBinding();
  sourceBindingPreparedForLoad = status == SourceBindingStatus::Match;
  return status;
}

bool Epub::bindCurrentSource() const {
  if (!ensureSourceIdentitySnapshot()) return false;
  const SourceIdentityStore::SaveStatus saved = SourceIdentityStore::save(cachePath, sourceIdentitySnapshot);
  return saved == SourceIdentityStore::SaveStatus::Saved || saved == SourceIdentityStore::SaveStatus::Unchanged;
}

BookMetadataCache::LoadStatus Epub::inspectCache() {
  if (!ensureSourceIdentitySnapshot()) return BookMetadataCache::LoadStatus::IoError;
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "Not enough memory to inspect EPUB cache");
    return BookMetadataCache::LoadStatus::IoError;
  }
  return bookMetadataCache->load(sourceIdentitySnapshot);
}

BookMetadataCache::LoadStepResult Epub::beginCacheInspection() {
  if (!ensureSourceIdentitySnapshot()) return BookMetadataCache::LoadStepResult::Error;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) return BookMetadataCache::LoadStepResult::Loaded;
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "Not enough memory to inspect EPUB cache");
    return BookMetadataCache::LoadStepResult::Error;
  }
  return bookMetadataCache->beginLoad(sourceIdentitySnapshot);
}

BookMetadataCache::LoadStepResult Epub::stepCacheInspection(const size_t maxEntries) {
  if (!bookMetadataCache) return BookMetadataCache::LoadStepResult::Error;
  return bookMetadataCache->stepLoad(maxEntries);
}

void Epub::cancelCacheInspection() {
  if (bookMetadataCache) bookMetadataCache->cancelLoad();
}

BookMetadataCache::LoadStatus Epub::getCacheLoadStatus() const {
  return bookMetadataCache ? bookMetadataCache->getLastLoadStatus() : BookMetadataCache::LoadStatus::IoError;
}

bool Epub::readCoreMetadata(BookMetadataCache::BookMetadata& metadata) {
  if (!beginCoreMetadataRead()) return false;
  CoreMetadataStepResult result = CoreMetadataStepResult::InProgress;
  while (result == CoreMetadataStepResult::InProgress) result = stepCoreMetadataRead(metadata);
  return result == CoreMetadataStepResult::Loaded;
}

bool Epub::beginCoreMetadataRead() {
  if (isReadingCoreMetadata() || isIndexing()) return false;
  sourceIdentityHandoff = {};
  auto state = makeUniqueNoThrow<CoreMetadataReadState>();
  if (!state) {
    LOG_ERR("EBP", "Not enough memory for cooperative EPUB metadata");
    return false;
  }
  if (hasSourceIdentitySnapshot) {
    state->initialIdentity = sourceIdentitySnapshot;
    state->phase = CoreMetadataReadState::Phase::Cache;
  } else if (!state->identityJob.begin(filepath)) {
    LOG_ERR("EBP", "Could not begin EPUB metadata source check");
    return false;
  }
  coreMetadataReadState = std::move(state);
  return true;
}

Epub::CoreMetadataStepResult Epub::stepCoreMetadataRead(BookMetadataCache::BookMetadata& metadata) {
  if (!coreMetadataReadState) return CoreMetadataStepResult::Error;
  auto& state = *coreMetadataReadState;
  const auto fail = [this](const char* message) {
    (void)message;
    LOG_ERR("EBP", "%s", message);
    cancelCoreMetadataRead();
    return CoreMetadataStepResult::Error;
  };
  const auto beginContainer = [this, &state, &fail]() {
    constexpr char containerPath[] = "META-INF/container.xml";
    bookMetadataCache.reset();
    state.metadata = {};
    size_t containerSize = 0;
    if (!getItemSize(containerPath, &containerSize)) return fail("Could not size container.xml metadata");
    state.containerParser = makeUniqueNoThrow<ContainerParser>(containerSize);
    if (!state.containerParser || !state.containerParser->setup() ||
        state.streamJob.begin(filepath, containerPath, *state.containerParser, 1024, containerSize, true) !=
            ZipStreamReadJob::BeginStatus::Started) {
      return fail("Could not begin cooperative container.xml metadata read");
    }
    state.phase = CoreMetadataReadState::Phase::Container;
    return CoreMetadataStepResult::InProgress;
  };

  switch (state.phase) {
    case CoreMetadataReadState::Phase::Source: {
      ZipFile::SourceIdentity identity;
      const auto status = state.identityJob.step(16U * 1024U, identity);
      if (status == ZipSourceIdentityJob::StepStatus::InProgress) return CoreMetadataStepResult::InProgress;
      if (status != ZipSourceIdentityJob::StepStatus::Done) return fail("Could not identify EPUB metadata source");
      sourceIdentitySnapshot = identity;
      hasSourceIdentitySnapshot = true;
      state.initialIdentity = identity;
      state.phase = CoreMetadataReadState::Phase::Cache;
      return CoreMetadataStepResult::InProgress;
    }

    case CoreMetadataReadState::Phase::Cache: {
      BookMetadataCache::LoadStepResult loadResult = BookMetadataCache::LoadStepResult::Error;
      if (bookMetadataCache && bookMetadataCache->isLoaded()) {
        loadResult = BookMetadataCache::LoadStepResult::Loaded;
      } else if (!state.cacheLoadActive) {
        if (!Storage.exists((cachePath + "/book.bin").c_str())) return beginContainer();
        bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
        if (!bookMetadataCache) {
          LOG_ERR("EBP", "Not enough memory for cached metadata; parsing OPF instead");
          return beginContainer();
        }
        loadResult = bookMetadataCache->beginLoad(state.initialIdentity);
        state.cacheLoadActive = loadResult == BookMetadataCache::LoadStepResult::InProgress;
      } else {
        loadResult = bookMetadataCache->stepLoad(8);
        state.cacheLoadActive = loadResult == BookMetadataCache::LoadStepResult::InProgress;
      }
      if (loadResult == BookMetadataCache::LoadStepResult::InProgress) return CoreMetadataStepResult::InProgress;
      if (loadResult != BookMetadataCache::LoadStepResult::Loaded ||
          bookMetadataCache->coreMetadata.coverItemHref.empty()) {
        return beginContainer();
      }
      state.metadata = bookMetadataCache->coreMetadata;
      coverResolutionComplete = true;
      state.phase = CoreMetadataReadState::Phase::Verify;
      return CoreMetadataStepResult::InProgress;
    }

    case CoreMetadataReadState::Phase::Container: {
      const auto readStatus = state.streamJob.step();
      if (readStatus == ZipStreamReadJob::StepStatus::InProgress) return CoreMetadataStepResult::InProgress;
      if (readStatus != ZipStreamReadJob::StepStatus::Done || !state.containerParser ||
          state.containerParser->fullPath.empty()) {
        return fail("Could not stream container.xml metadata");
      }
      state.entryPath = std::move(state.containerParser->fullPath);
      state.containerParser.reset();
      contentBasePath = state.entryPath.substr(0, state.entryPath.find_last_of('/') + 1);

      size_t opfSize = 0;
      if (!getItemSize(state.entryPath, &opfSize)) return fail("Could not size content.opf metadata");
      state.opfParser = makeUniqueNoThrow<ContentOpfParser>(getCachePath(), getBasePath(), opfSize, nullptr);
      if (!state.opfParser || !state.opfParser->setup() ||
          state.streamJob.begin(filepath, state.entryPath.c_str(), *state.opfParser, 1024, opfSize, true) !=
              ZipStreamReadJob::BeginStatus::Started) {
        return fail("Could not begin cooperative content.opf metadata read");
      }
      state.phase = CoreMetadataReadState::Phase::Opf;
      return CoreMetadataStepResult::InProgress;
    }

    case CoreMetadataReadState::Phase::Opf: {
      const auto readStatus = state.streamJob.step();
      if (readStatus == ZipStreamReadJob::StepStatus::InProgress) return CoreMetadataStepResult::InProgress;
      if (readStatus != ZipStreamReadJob::StepStatus::Done || !state.opfParser || !state.opfParser->succeeded()) {
        return fail("Could not stream content.opf metadata");
      }
      state.guidePath = state.opfParser->guideCoverPageHref;
      if (!finalizeContentOpf(*state.opfParser, state.metadata, /*resolveGuideCover=*/false)) {
        return fail("Could not finalize content.opf metadata");
      }
      state.opfParser.reset();
      if (state.metadata.coverItemHref.empty() && !state.guidePath.empty()) {
        if (!getItemSize(state.guidePath, &state.guideSize)) {
          LOG_ERR("EBP", "Could not size guide cover page");
        } else if (state.guideSize > GUIDE_COVER_PAGE_MAX_BYTES) {
          LOG_ERR("EBP", "Guide cover page is too large (%zu bytes)", state.guideSize);
        } else if (state.guideSize == 0) {
          resolveGuideCover(state.guidePath, nullptr, 0, state.metadata);
        } else {
          state.guideBuffer = makeUniqueNoThrow<uint8_t[]>(state.guideSize);
          if (state.guideBuffer &&
              state.streamJob.begin(filepath, state.guidePath.c_str(), state, 1024, state.guideSize, true) ==
                  ZipStreamReadJob::BeginStatus::Started) {
            state.phase = CoreMetadataReadState::Phase::Guide;
            return CoreMetadataStepResult::InProgress;
          }
          LOG_ERR("EBP", "Could not begin cooperative guide cover read");
          state.guideBuffer.reset();
        }
      }
      state.phase = CoreMetadataReadState::Phase::Verify;
      return CoreMetadataStepResult::InProgress;
    }

    case CoreMetadataReadState::Phase::Guide: {
      const auto readStatus = state.streamJob.step();
      if (readStatus == ZipStreamReadJob::StepStatus::InProgress) return CoreMetadataStepResult::InProgress;
      if (readStatus == ZipStreamReadJob::StepStatus::Done && state.guideWritten == state.guideSize) {
        resolveGuideCover(state.guidePath, state.guideBuffer.get(), state.guideSize, state.metadata);
      } else {
        LOG_ERR("EBP", "Could not stream guide cover metadata");
      }
      state.guideBuffer.reset();
      state.phase = CoreMetadataReadState::Phase::Verify;
      return CoreMetadataStepResult::InProgress;
    }

    case CoreMetadataReadState::Phase::Verify: {
      if (!state.verifyStarted) {
        if (!state.identityJob.begin(filepath)) return fail("Could not begin final EPUB metadata source check");
        state.verifyStarted = true;
        return CoreMetadataStepResult::InProgress;
      }
      ZipFile::SourceIdentity finalIdentity;
      ZipSourceIdentityJob::FileStamp fileStamp;
      const auto status = state.identityJob.step(16U * 1024U, finalIdentity, &fileStamp);
      if (status == ZipSourceIdentityJob::StepStatus::InProgress) return CoreMetadataStepResult::InProgress;
      if (status != ZipSourceIdentityJob::StepStatus::Done || finalIdentity != state.initialIdentity) {
        return fail("EPUB changed while core metadata was loading");
      }
      if (!fileStamp.valid ||
          !sourceIdentityHandoff.captureVerified(filepath, finalIdentity, fileStamp.modifyDate, fileStamp.modifyTime)) {
        sourceIdentityHandoff = {};
      }
      metadata = state.metadata;
      transientMetadata = state.metadata;
      hasTransientMetadata = true;
      coreMetadataReadState.reset();
      return CoreMetadataStepResult::Loaded;
    }
  }
  return fail("Invalid cooperative EPUB metadata state");
}

void Epub::cancelCoreMetadataRead() {
  if (!coreMetadataReadState) return;
  coreMetadataReadState->identityJob.cancel();
  coreMetadataReadState->streamJob.cancel();
  if (coreMetadataReadState->cacheLoadActive && bookMetadataCache) bookMetadataCache->cancelLoad();
  coreMetadataReadState.reset();
}

bool Epub::isReadingCoreMetadata() const { return coreMetadataReadState != nullptr; }

bool Epub::hasPreparedCoreMetadata() const {
  return hasTransientMetadata || (bookMetadataCache && bookMetadataCache->isLoaded());
}

bool Epub::getSourceIdentityHandoff(RawSourceIdentityHandoff& handoff) const {
  if (!hasSourceIdentitySnapshot || !sourceIdentityHandoff.valid || sourceIdentityHandoff.path != filepath ||
      sourceIdentityHandoff.identity != sourceIdentitySnapshot) {
    return false;
  }
  handoff = sourceIdentityHandoff;
  return true;
}

bool Epub::prepareForReaderLoadAfterRecovery(const ZipFile::SourceIdentity& verifiedSourceIdentity) {
  if (!hasSourceIdentitySnapshot || sourceIdentitySnapshot != verifiedSourceIdentity || isReadingCoreMetadata() ||
      isIndexing()) {
    return false;
  }
  sourceReplacementRecoveryDone = true;
  return true;
}

bool Epub::prepareCssCache(const bool verifySourceAtEntry) {
  if (!cssParser || !bookMetadataCache || (verifySourceAtEntry && !sourceStillMatchesSnapshot())) {
    LOG_ERR("EBP", "Cannot prepare CSS cache without a loaded, matching EPUB");
    return false;
  }

  // Materialize while validating when memory permits so the first section
  // build can reuse this exact read. Under low heap loadFromCache() rejects
  // before rule allocation and preserves the file, so retain the validation-
  // only fallback that lets an already-cached section remain readable.
  if (cssParser->loadFromCache() || (cssParser->hasCache() && cssParser->validateCache())) {
    externalCssUnavailable = false;
    return true;
  }

  cssParser->clear();

  // Section headers do not encode the CSS cache version, so every failed
  // cache read must invalidate them before a rebuild is attempted. This also
  // prevents a best-effort book load from accepting stale rendered pages when
  // external CSS itself cannot be recovered.
  const std::string sectionsPath = cachePath + "/sections";
  if (Storage.exists(sectionsPath.c_str()) &&
      (!Storage.removeDir(sectionsPath.c_str()) || Storage.exists(sectionsPath.c_str()))) {
    LOG_ERR("EBP", "Could not invalidate sections before rebuilding CSS cache");
    return false;
  }

  cssParser->deleteCache();
  if (cssParser->hasCache()) {
    LOG_ERR("EBP", "Could not remove invalid CSS cache");
    return false;
  }

  // A warm load skipped OPF parsing when styles were disabled. Recover the
  // stylesheet list lazily; during first-time indexing it is already known.
  if (bookMetadataCache->isLoaded()) {
    cssFiles.clear();
    BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
    if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
      LOG_ERR("EBP", "Could not parse content.opf while preparing CSS cache");
      return false;
    }
    discoverCssFilesFromZip();
  }

  // Parsing CSS is memory-heavy. Temporarily release book.bin, then restore it
  // even on a failed cache write so the current reading session remains usable.
  bookMetadataCache.reset();
  const bool saved = parseCssFiles();
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  const bool metadataReloaded =
      bookMetadataCache && bookMetadataCache->load(sourceIdentitySnapshot) == BookMetadataCache::LoadStatus::Loaded;
  if (!bookMetadataCache) LOG_ERR("EBP", "Not enough memory to reload metadata after CSS parsing");

  const bool sourceReady = saved && metadataReloaded && sourceStillMatchesSnapshot();
  const bool cacheReady =
      sourceReady && (cssParser->loadFromCache() || (cssParser->hasCache() && cssParser->validateCache()));
  if (!cacheReady) {
    LOG_ERR("EBP", "Failed to build and verify CSS cache");
    cssParser->clear();
    cssParser->deleteCache();
    return false;
  }

  externalCssUnavailable = false;
  return true;
}

bool Epub::ensureCssCache() { return prepareCssCache(true); }

bool Epub::prepareCssForLoad(const bool skipLoadingCss) {
  // The caller already verified the source before reading derived data and
  // verifies it again before returning. Avoid a redundant full central-
  // directory hash here while retaining both safety boundaries.
  if (skipLoadingCss || prepareCssCache(false)) return true;

  // External CSS is best-effort while opening a book. Keep the EPUB usable
  // only when its authoritative metadata/source remain valid and no stale
  // rendered section survived the failed cache rebuild.
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || !sourceStillMatchesSnapshot() ||
      Storage.exists((cachePath + "/sections").c_str())) {
    return false;
  }
  externalCssUnavailable = true;
  LOG_ERR("EBP", "Continuing without external CSS cache");
  return true;
}

bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  return loadImpl(buildIfMissing, skipLoadingCss, true);
}

bool Epub::loadForCooperativeSourceCheck(const bool buildIfMissing, const bool skipLoadingCss) {
  return loadImpl(buildIfMissing, skipLoadingCss, false);
}

// load in the meta data for the epub file
bool Epub::loadImpl(const bool buildIfMissing, const bool skipLoadingCss, const bool verifySourceAtReturn) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  EpubLoadDebugMetric debugLoadMetric;
#endif
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  externalCssUnavailable = false;

  // The durable sidecar survives a derived-cache clear. ReaderActivity may
  // have prepared this exact load already; consume that proof once rather than
  // reopening all three sidecar candidates. Every success path still checks
  // the current EPUB against the source snapshot before returning.
  const bool sourceBindingAlreadyVerified = sourceBindingPreparedForLoad;
  sourceBindingPreparedForLoad = false;
  if (!sourceBindingAlreadyVerified && inspectSourceBinding() != SourceBindingStatus::Match) return false;

  // ReaderActivity may already have inspected and fully loaded this exact
  // source-bound cache while choosing the loading flow. Reuse it instead of
  // validating and materializing book.bin a second time.
  const bool reuseInspectedMetadata = bookMetadataCache && bookMetadataCache->isLoaded();
  if (!reuseInspectedMetadata) bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!bookMetadataCache || !cssParser) {
    LOG_ERR("EBP", "Not enough memory to initialize EPUB metadata and CSS");
    bookMetadataCache.reset();
    cssParser.reset();
    return false;
  }
  // Try to load existing cache first
  const BookMetadataCache::LoadStatus cacheStatus =
      reuseInspectedMetadata ? BookMetadataCache::LoadStatus::Loaded : bookMetadataCache->load(sourceIdentitySnapshot);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  debugLoadMetric.classifyCache(cacheStatus, buildIfMissing);
#endif
  if (cacheStatus == BookMetadataCache::LoadStatus::Loaded) {
    if (!prepareCssForLoad(skipLoadingCss)) return false;
    if (verifySourceAtReturn && !sourceStillMatchesSnapshot()) {
      LOG_ERR("EBP", "EPUB changed while cache was loading");
      return false;
    }
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    debugLoadMetric.markSuccess();
#endif
    return true;
  }

  // The durable sidecar above is authoritative for user state. A mismatched
  // book.bin is therefore only stale derived data and may be rebuilt. Newer
  // formats and I/O failures remain protected from downgrade/rewrite.
  if (cacheStatus == BookMetadataCache::LoadStatus::NewerVersion ||
      cacheStatus == BookMetadataCache::LoadStatus::IoError) {
    return false;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  if (!beginColdIndexing(skipLoadingCss)) return false;
  IndexStepResult result = IndexStepResult::InProgress;
  while (result == IndexStepResult::InProgress) result = stepIndexing();
  if (result != IndexStepResult::Loaded) return false;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  debugLoadMetric.markSuccess();
#endif
  return true;
}

bool Epub::beginIndexing(const bool skipLoadingCss) {
  if (isIndexing()) return false;

  LOG_DBG("EBP", "Beginning cooperative EPUB indexing: %s", filepath.c_str());
  externalCssUnavailable = false;

  const bool sourceBindingAlreadyVerified = sourceBindingPreparedForLoad;
  sourceBindingPreparedForLoad = false;
  if (!sourceBindingAlreadyVerified && inspectSourceBinding() != SourceBindingStatus::Match) return false;

  BookMetadataCache::LoadStatus cacheStatus = BookMetadataCache::LoadStatus::Missing;
  if (bookMetadataCache) {
    cacheStatus =
        bookMetadataCache->isLoaded() ? BookMetadataCache::LoadStatus::Loaded : bookMetadataCache->getLastLoadStatus();
  } else {
    bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
    if (bookMetadataCache) cacheStatus = bookMetadataCache->load(sourceIdentitySnapshot);
  }
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!bookMetadataCache || !cssParser) {
    LOG_ERR("EBP", "Not enough memory to initialize EPUB metadata and CSS");
    bookMetadataCache.reset();
    cssParser.reset();
    return false;
  }

  if (cacheStatus == BookMetadataCache::LoadStatus::Loaded) {
    LOG_DBG("EBP", "Cooperative indexing requested for an already loaded cache");
    return false;
  }
  if (cacheStatus == BookMetadataCache::LoadStatus::NewerVersion ||
      cacheStatus == BookMetadataCache::LoadStatus::IoError) {
    return false;
  }
  return beginColdIndexing(skipLoadingCss);
}

bool Epub::beginColdIndexing(const bool skipLoadingCss) {
  if (isIndexing() || !bookMetadataCache || !cssParser) return false;

  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  if (!setupCacheDir()) return false;

  // Bind the whole indexing attempt to one source snapshot. buildBookBin()
  // rechecks this before publishing, and the final load checks it once more.
  if (!ensureSourceIdentitySnapshot()) {
    LOG_ERR("EBP", "Could not identify EPUB before indexing");
    return false;
  }

  // Any metadata rebuild invalidates all section/CSS output, even when CSS is
  // disabled. User state lives in separate files and is deliberately retained
  // for legacy/invalid derived caches.
  const std::string sectionsPath = cachePath + "/sections";
  if (Storage.exists(sectionsPath.c_str()) && !Storage.removeDir(sectionsPath.c_str())) {
    LOG_ERR("EBP", "Could not invalidate stale section cache");
    return false;
  }
  cssParser->deleteCache();
  if (cssParser->hasCache()) {
    LOG_ERR("EBP", "Could not invalidate stale CSS cache");
    return false;
  }

  tocNcxItem.clear();
  tocNavItem.clear();
  contentBasePath.clear();
  cssFiles.clear();
  cssDiscoveryComplete = true;
  indexingMetadata = {};
  indexingSkipLoadingCss = skipLoadingCss;
  indexingReadState.reset();
  indexingSourceIdentityJob.reset();
  indexingCacheReloadActive = false;
  indexingBookBuilt = false;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  indexingStartedMs = static_cast<uint32_t>(millis());
#else
  indexingStartedMs = 0;
#endif

  // Stream parser output to scratch files immediately. The final book.bin is
  // not published until both passes have completed and been validated.
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }
  indexingPhase = IndexingPhase::Opf;
  indexingPhaseStartedMs = static_cast<uint32_t>(millis());
  return true;
}

Epub::IndexStepResult Epub::stepIndexing() {
  const auto fail = [this](const char* message) {
    (void)message;
    LOG_ERR("EBP", "%s", message);
    cancelIndexing();
    return IndexStepResult::Error;
  };

  switch (indexingPhase) {
    case IndexingPhase::Idle:
      return IndexStepResult::Error;

    case IndexingPhase::Opf: {
      constexpr char containerPath[] = "META-INF/container.xml";
      if (!indexingReadState) {
        if (!bookMetadataCache->beginContentOpfPass()) {
          return fail("Could not begin writing content.opf pass");
        }
        size_t containerSize = 0;
        if (!getItemSize(containerPath, &containerSize)) return fail("Could not size container.xml");

        indexingReadState = makeUniqueNoThrow<IndexingReadState>();
        if (!indexingReadState) return fail("Not enough memory for cooperative container parser");
        indexingReadState->kind = IndexingReadState::Kind::Container;
        indexingReadState->entryPath = containerPath;
        indexingReadState->containerParser = makeUniqueNoThrow<ContainerParser>(containerSize);
        if (!indexingReadState->containerParser || !indexingReadState->containerParser->setup() ||
            indexingReadState->job.begin(filepath, containerPath, *indexingReadState->containerParser, 1024,
                                         containerSize, true) != ZipStreamReadJob::BeginStatus::Started) {
          return fail("Could not begin cooperative container.xml read");
        }
        return IndexStepResult::InProgress;
      }

      const ZipStreamReadJob::StepStatus readStatus = indexingReadState->job.step();
      if (readStatus == ZipStreamReadJob::StepStatus::InProgress) return IndexStepResult::InProgress;
      if (readStatus == ZipStreamReadJob::StepStatus::Error) return fail("Could not stream EPUB metadata");

      if (indexingReadState->kind == IndexingReadState::Kind::Container) {
        if (!indexingReadState->containerParser || indexingReadState->containerParser->fullPath.empty()) {
          return fail("Could not find valid rootfile in container.xml");
        }
        indexingReadState->entryPath = std::move(indexingReadState->containerParser->fullPath);
        indexingReadState->containerParser.reset();
        contentBasePath = indexingReadState->entryPath.substr(0, indexingReadState->entryPath.find_last_of('/') + 1);

        size_t opfSize = 0;
        if (!getItemSize(indexingReadState->entryPath, &opfSize)) return fail("Could not size content.opf");
        indexingReadState->opfParser =
            makeUniqueNoThrow<ContentOpfParser>(getCachePath(), getBasePath(), opfSize, bookMetadataCache.get());
        if (!indexingReadState->opfParser || !indexingReadState->opfParser->setup() ||
            indexingReadState->job.begin(filepath, indexingReadState->entryPath.c_str(), *indexingReadState->opfParser,
                                         1024, opfSize, true) != ZipStreamReadJob::BeginStatus::Started) {
          return fail("Could not begin cooperative content.opf read");
        }
        indexingReadState->kind = IndexingReadState::Kind::Opf;
        return IndexStepResult::InProgress;
      }

      if (indexingReadState->kind != IndexingReadState::Kind::Opf || !indexingReadState->opfParser ||
          !indexingReadState->opfParser->succeeded() ||
          !finalizeContentOpf(*indexingReadState->opfParser, indexingMetadata)) {
        return fail("Could not parse content.opf");
      }
      indexingReadState.reset();
      discoverCssFilesFromZip();
      if (!bookMetadataCache->endContentOpfPass()) return fail("Could not end writing content.opf pass");
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      LOG_DBG("EBP", "OPF pass completed in %u ms",
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingPhaseStartedMs));
#endif
      indexingPhase = IndexingPhase::Toc;
      indexingPhaseStartedMs = static_cast<uint32_t>(millis());
      return IndexStepResult::InProgress;
    }

    case IndexingPhase::Toc: {
      const auto beginTocRead = [this](const bool nav) {
        const std::string& item = nav ? tocNavItem : tocNcxItem;
        if (item.empty()) return false;
        size_t itemSize = 0;
        if (!getItemSize(item, &itemSize)) return false;

        indexingReadState = makeUniqueNoThrow<IndexingReadState>();
        if (!indexingReadState) return false;
        indexingReadState->entryPath = item;
        Print* parser = nullptr;
        indexingReadState->basePath = item.substr(0, item.find_last_of('/') + 1);
        if (nav) {
          indexingReadState->kind = IndexingReadState::Kind::TocNav;
          indexingReadState->navParser =
              makeUniqueNoThrow<TocNavParser>(indexingReadState->basePath, itemSize, bookMetadataCache.get());
          if (!indexingReadState->navParser || !indexingReadState->navParser->setup()) return false;
          parser = indexingReadState->navParser.get();
        } else {
          indexingReadState->kind = IndexingReadState::Kind::TocNcx;
          indexingReadState->ncxParser =
              makeUniqueNoThrow<TocNcxParser>(indexingReadState->basePath, itemSize, bookMetadataCache.get());
          if (!indexingReadState->ncxParser || !indexingReadState->ncxParser->setup()) return false;
          parser = indexingReadState->ncxParser.get();
        }
        if (indexingReadState->job.begin(filepath, item.c_str(), *parser, 1024, itemSize, true) !=
            ZipStreamReadJob::BeginStatus::Started) {
          indexingReadState.reset();
          return false;
        }
        return true;
      };
      const auto finishTocPass = [this, &fail](const bool parsed) {
        indexingReadState.reset();
        if (!parsed) LOG_ERR("EBP", "Warning: Could not parse any TOC format");
        if (!bookMetadataCache->endTocPass()) return fail("Could not end writing toc pass");
        if (!bookMetadataCache->endWrite()) return fail("Could not end writing cache");
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        LOG_DBG("EBP", "TOC pass completed in %u ms",
                static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingPhaseStartedMs));
#endif
        indexingPhase = IndexingPhase::BuildBook;
        indexingPhaseStartedMs = static_cast<uint32_t>(millis());
        return IndexStepResult::InProgress;
      };

      if (!indexingReadState) {
        if (!bookMetadataCache->beginTocPass()) return fail("Could not begin writing toc pass");
        if (!tocNavItem.empty()) {
          LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
          if (beginTocRead(true)) return IndexStepResult::InProgress;
        }
        if (!tocNcxItem.empty()) {
          LOG_DBG("EBP", "Falling back to NCX TOC");
          if (beginTocRead(false)) return IndexStepResult::InProgress;
        }
        return finishTocPass(false);
      }

      const IndexingReadState::Kind kind = indexingReadState->kind;
      const ZipStreamReadJob::StepStatus readStatus = indexingReadState->job.step();
      if (readStatus == ZipStreamReadJob::StepStatus::InProgress) return IndexStepResult::InProgress;

      const bool navUsable = kind == IndexingReadState::Kind::TocNav && indexingReadState->navParser &&
                             indexingReadState->navParser->succeeded() &&
                             indexingReadState->navParser->usableEntryCount() > 0;
      const bool ncxUsable = kind == IndexingReadState::Kind::TocNcx && indexingReadState->ncxParser &&
                             indexingReadState->ncxParser->succeeded();
      if (readStatus == ZipStreamReadJob::StepStatus::Done && (navUsable || ncxUsable)) {
        return finishTocPass(true);
      }

      indexingReadState.reset();
      if (kind == IndexingReadState::Kind::TocNav && !tocNcxItem.empty()) {
        LOG_DBG("EBP", "Falling back to NCX TOC");
        if (!bookMetadataCache->restartTocPass()) return fail("Could not reset toc pass for NCX fallback");
        if (beginTocRead(false)) return IndexStepResult::InProgress;
      }
      return finishTocPass(false);
    }

    case IndexingPhase::BuildBook: {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      const uint32_t started = static_cast<uint32_t>(millis());
#endif
      if (!bookMetadataCache->isBuildingBookBin()) {
        if (!bookMetadataCache->beginBuildBookBin(filepath, indexingMetadata, sourceIdentitySnapshot)) {
          return fail("Could not begin updating mappings and sizes");
        }
        return IndexStepResult::InProgress;
      }
      const BookMetadataCache::BuildStepResult buildResult = bookMetadataCache->stepBuildBookBin(8);
      if (buildResult == BookMetadataCache::BuildStepResult::InProgress) return IndexStepResult::InProgress;
      if (buildResult == BookMetadataCache::BuildStepResult::Error) return fail("Could not update mappings and sizes");
      indexingBookBuilt = true;
      if (!bookMetadataCache->cleanupTmpFiles()) {
        LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
      }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      LOG_DBG("EBP", "buildBookBin completed in %u ms",
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - started));
#endif
      indexingPhase = IndexingPhase::Css;
      indexingPhaseStartedMs = static_cast<uint32_t>(millis());
      return IndexStepResult::InProgress;
    }

    case IndexingPhase::Css:
      if (!prepareCssForLoad(indexingSkipLoadingCss)) return fail("Could not prepare EPUB styles");
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      LOG_DBG("EBP", "CSS phase completed in %u ms",
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingPhaseStartedMs));
#endif
      indexingPhase = IndexingPhase::Reload;
      indexingPhaseStartedMs = static_cast<uint32_t>(millis());
      return IndexStepResult::InProgress;

    case IndexingPhase::Reload: {
      // CSS preparation reloads book.bin after temporarily lending its memory
      // to parsing. A style-free load still needs the normal first reload here.
      if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
        if (!indexingCacheReloadActive) {
          bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
          if (!bookMetadataCache) return fail("Not enough memory to reload EPUB cache after indexing");
          const BookMetadataCache::LoadStepResult result = bookMetadataCache->beginLoad(sourceIdentitySnapshot);
          if (result == BookMetadataCache::LoadStepResult::Error) {
            return fail("Failed to begin cache reload after writing");
          }
          indexingCacheReloadActive = result == BookMetadataCache::LoadStepResult::InProgress;
          if (indexingCacheReloadActive) return IndexStepResult::InProgress;
        } else {
          const BookMetadataCache::LoadStepResult result = bookMetadataCache->stepLoad(8);
          if (result == BookMetadataCache::LoadStepResult::InProgress) return IndexStepResult::InProgress;
          indexingCacheReloadActive = false;
          if (result == BookMetadataCache::LoadStepResult::Error) {
            return fail("Failed to reload cache after writing");
          }
        }
      }
      if (!bookMetadataCache->isLoaded()) return fail("Failed to reload cache after writing");

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      LOG_DBG("EBP", "Cache reload completed in %u ms",
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingPhaseStartedMs));
#endif
      indexingSourceIdentityJob = makeUniqueNoThrow<ZipSourceIdentityJob>();
      if (!indexingSourceIdentityJob || !indexingSourceIdentityJob->begin(filepath)) {
        return fail("Could not begin final EPUB identity check");
      }
      indexingPhase = IndexingPhase::SourceCheck;
      indexingPhaseStartedMs = static_cast<uint32_t>(millis());
      return IndexStepResult::InProgress;
    }

    case IndexingPhase::SourceCheck: {
      if (!indexingSourceIdentityJob) return fail("Final EPUB identity check is missing");
      ZipFile::SourceIdentity currentIdentity;
      const ZipSourceIdentityJob::StepStatus result = indexingSourceIdentityJob->step(16U * 1024U, currentIdentity);
      if (result == ZipSourceIdentityJob::StepStatus::InProgress) return IndexStepResult::InProgress;
      indexingSourceIdentityJob.reset();
      if (result != ZipSourceIdentityJob::StepStatus::Done || currentIdentity != sourceIdentitySnapshot) {
        return fail("EPUB changed while indexing");
      }

      indexingPhase = IndexingPhase::Idle;
      indexingReadState.reset();
      indexingMetadata = {};
      indexingSkipLoadingCss = false;
      indexingCacheReloadActive = false;
      indexingBookBuilt = false;
      LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      LOG_DBG("EBP", "Total cooperative indexing completed in %u ms",
              static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingStartedMs));
#endif
      indexingStartedMs = 0;
      indexingPhaseStartedMs = 0;
      return IndexStepResult::Loaded;
    }
  }

  return fail("Invalid EPUB indexing state");
}

void Epub::cancelIndexing() {
  if (!isIndexing()) return;
  indexingReadState.reset();
  if (indexingSourceIdentityJob) indexingSourceIdentityJob->cancel();
  indexingSourceIdentityJob.reset();
  if (bookMetadataCache) {
    if (indexingCacheReloadActive) bookMetadataCache->cancelLoad();
    bookMetadataCache->cancelBuildBookBin();
    bookMetadataCache->cancelWrite();
  }
  if (indexingBookBuilt) Storage.remove((cachePath + "/book.bin").c_str());
  indexingPhase = IndexingPhase::Idle;
  indexingMetadata = {};
  indexingSkipLoadingCss = false;
  indexingCacheReloadActive = false;
  indexingBookBuilt = false;
  indexingStartedMs = 0;
  indexingPhaseStartedMs = 0;
}

bool Epub::isIndexing() const { return indexingPhase != IndexingPhase::Idle; }

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

bool Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to create cache directory: %s", cachePath.c_str());
    return false;
  }
  return true;
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

const std::string& Epub::getCoverItemHref() const {
  static const std::string blank;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) return bookMetadataCache->coreMetadata.coverItemHref;
  return hasTransientMetadata ? transientMetadata.coverItemHref : blank;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::openCoverSource(const std::string& coverImageHref, const bool jpeg, CoverSource& source) const {
  source = {};
  if (jpeg) {
    const std::string entryPath = FsHelpers::normalisePath(coverImageHref);
    const ZipFile::StoredEntryOpenStatus status =
        ZipFile(filepath).openStoredEntry(entryPath.c_str(), source.file, source.offset, source.length);
    if (status == ZipFile::StoredEntryOpenStatus::Opened) {
      source.ranged = true;
      return true;
    }
    if (status == ZipFile::StoredEntryOpenStatus::Invalid || status == ZipFile::StoredEntryOpenStatus::IoError) {
      return false;
    }
  }

  const std::string requestedPath = getCachePath() + (jpeg ? "/.cover.jpg" : "/.cover.png");
  if (coverSourcePath == requestedPath && Storage.exists(requestedPath.c_str())) {
    if (hasSourceIdentitySnapshot && !sourceStillMatchesSnapshot()) {
      clearCoverSource();
      return false;
    }
    if (Storage.openFileForRead("EBP", requestedPath, source.file)) return true;
    clearCoverSource();
    return false;
  }

  clearCoverSource();
  if (!coverSourcePath.empty()) return false;
  coverSourcePath = requestedPath;
  if (Storage.exists(requestedPath.c_str()) && !Storage.remove(requestedPath.c_str())) return false;
  HalFile extractedSource;
  if (!Storage.openFileForWrite("EBP", requestedPath, extractedSource)) {
    clearCoverSource();
    return false;
  }
  constexpr size_t MAX_EXTRACTED_COVER_BYTES = 16U * 1024U * 1024U;
  const bool extracted =
      readItemContentsToStream(coverImageHref, extractedSource, 4096, false, MAX_EXTRACTED_COVER_BYTES);
  const bool synced = extractedSource.sync();
  const bool closed = extractedSource.close();
  if (!extracted || !synced || !closed) {
    clearCoverSource();
    return false;
  }

  if (Storage.openFileForRead("EBP", requestedPath, source.file)) return true;
  clearCoverSource();
  return false;
}

void Epub::clearCoverSource() const {
  if (coverSourcePath.empty()) return;
  if (!Storage.exists(coverSourcePath.c_str()) || Storage.remove(coverSourcePath.c_str())) {
    coverSourcePath.clear();
  } else {
    LOG_ERR("EBP", "Failed to remove cover scratch file: %s", coverSourcePath.c_str());
  }
}

bool Epub::generateCoverBmp(bool cropped) const {
  const std::string finalPath = getCoverBmpPath(cropped);
  const std::string stagingPath = finalPath + ".tmp";
  const BitmapCacheState cacheState = Bitmap::inspectDerivedCache(finalPath);
  if (cacheState == BitmapCacheState::Ready) return true;
  if (cacheState == BitmapCacheState::IoError) return false;

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  const bool jpeg = FsHelpers::hasJpgExtension(coverImageHref);
  const bool png = FsHelpers::hasPngExtension(coverImageHref);
  if (!jpeg && !png) {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping");
    return false;
  }

  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;
  CoverSource source;
  if (!openCoverSource(coverImageHref, jpeg, source)) return false;
  HalFile output;
  if (!Storage.openFileForWrite("EBP", stagingPath, output)) {
    source.file.close();
    if (!retainCoverSource) clearCoverSource();
    return false;
  }
  const bool converted = jpeg ? (source.ranged ? JpegToBmpConverter::jpegFileRangeToBmpStream(
                                                     source.file, source.offset, source.length, output, cropped)
                                               : JpegToBmpConverter::jpegFileToBmpStream(source.file, output, cropped))
                              : PngToBmpConverter::pngFileToBmpStream(source.file, output, cropped);
  const bool outputSynced = output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = source.file.close();
  if (!retainCoverSource) clearCoverSource();
  if (!converted || !outputSynced || !outputClosed || !inputClosed || !publishBitmap(finalPath, stagingPath)) {
    Storage.remove(stagingPath.c_str());
    LOG_ERR("EBP", "Failed to generate BMP from cover image");
    return false;
  }
  LOG_DBG("EBP", "Generated BMP from %s cover image", jpeg ? "JPG" : "PNG");
  return true;
}

bool Epub::generateCoverBmp(const bool cropped, const ThumbnailRequest& thumbnails) {
  if (!ensureSourceIdentitySnapshot()) return false;
  const bool previousRetention = retainCoverSource;
  retainCoverSource = true;
  const bool coverReady = static_cast<const Epub*>(this)->generateCoverBmp(cropped);
  if (coverReady && !sourceStillMatchesSnapshot()) {
    Storage.remove(getCoverBmpPath(cropped).c_str());
    retainCoverSource = previousRetention;
    if (!retainCoverSource) clearCoverSource();
    return false;
  }
  ensureThumbnails(thumbnails, ThumbnailMode::EmbeddedThenCover);
  retainCoverSource = previousRetention;
  if (!retainCoverSource) clearCoverSource();
  return coverReady;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

const char* Epub::sharedThumbnailEntry() { return "META-INF/crossvi/cover-144x240-v1.bmp"; }

const char* Epub::carouselThumbnailEntry(const int width, const int height) {
  if (width == CAROUSEL_THUMB_WIDTH && height == CAROUSEL_THUMB_HEIGHT) {
    return "META-INF/crossvi/cover-273x456-v2.bmp";
  }
  if (width == CAROUSEL_X4_THUMB_WIDTH && height == CAROUSEL_X4_THUMB_HEIGHT) {
    return "META-INF/crossvi/cover-249x415-v2.bmp";
  }
  return nullptr;
}

Epub::ThumbnailStatus Epub::ensureSharedThumbnail(const ThumbnailMode mode) {
  return ensureThumbnail(SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT, sharedThumbnailEntry(), mode, false);
}

Epub::ThumbnailStatus Epub::ensureCarouselThumbnail(const int width, const int height, const ThumbnailMode mode) {
  return ensureThumbnail(width, height, carouselThumbnailEntry(width, height), mode, true);
}

bool Epub::generateJpegThumbnailPair(const int carouselWidth, const int carouselHeight, ThumbnailSetStatus& result) {
  const BookMetadataCache::BookMetadata* metadata = nullptr;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) {
    metadata = &bookMetadataCache->coreMetadata;
  } else if (hasTransientMetadata) {
    metadata = &transientMetadata;
  } else {
    BookMetadataCache::BookMetadata loaded;
    if (!readCoreMetadata(loaded)) return false;
    metadata = &transientMetadata;
  }
  if (!metadata || metadata->coverItemHref.empty() || !FsHelpers::hasJpgExtension(metadata->coverItemHref)) {
    return false;
  }

  if (!setupCacheDir()) {
    result.shared = ThumbnailStatus::IoError;
    result.carousel = ThumbnailStatus::IoError;
    return true;
  }
  const std::array<std::string, 2> finalPaths = {getThumbBmpPath(SHARED_THUMB_HEIGHT), getThumbBmpPath(carouselHeight)};
  const std::array<std::string, 2> stagingPaths = {finalPaths[0] + ".tmp", finalPaths[1] + ".tmp"};
  const std::array<std::string, 2> identityPaths = {finalPaths[0] + ".identity", finalPaths[1] + ".fit-v2.identity"};
  const std::array<std::string, 2> noCoverPaths = {finalPaths[0] + ".nocover", finalPaths[1] + ".nocover"};
  std::array<ThumbnailValidationContext, 2> validation = {
      ThumbnailValidationContext{SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT,
                                 thumbnailFileSize(SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT), false},
      ThumbnailValidationContext{carouselWidth, carouselHeight, thumbnailFileSize(carouselWidth, carouselHeight),
                                 true}};

  for (const auto& path : stagingPaths) {
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      result.shared = ThumbnailStatus::IoError;
      result.carousel = ThumbnailStatus::IoError;
      return true;
    }
  }

  std::array<HalFile, 2> outputs;
  for (size_t index = 0; index < outputs.size(); ++index) {
    if (!Storage.openFileForWrite("EBP", stagingPaths[index], outputs[index])) {
      for (size_t opened = 0; opened < index; ++opened) outputs[opened].close();
      for (const auto& path : stagingPaths) Storage.remove(path.c_str());
      result.shared = ThumbnailStatus::IoError;
      result.carousel = ThumbnailStatus::IoError;
      return true;
    }
  }

  CoverSource source;
  if (!openCoverSource(metadata->coverItemHref, true, source)) {
    for (auto& output : outputs) output.close();
    for (const auto& path : stagingPaths) Storage.remove(path.c_str());
    result.shared = ThumbnailStatus::IoError;
    result.carousel = result.shared;
    return true;
  }

  const std::array<JpegToBmpConverter::OneBitBmpTarget, 2> targets = {
      JpegToBmpConverter::OneBitBmpTarget{&outputs[0], SHARED_THUMB_WIDTH, SHARED_THUMB_HEIGHT, true},
      JpegToBmpConverter::OneBitBmpTarget{&outputs[1], carouselWidth, carouselHeight, false}};
  const uint8_t converted =
      source.ranged ? JpegToBmpConverter::jpegFileRangeTo1BitBmpStreamsWithSize(
                          source.file, source.offset, source.length, targets.data(), targets.size())
                    : JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSize(source.file, targets.data(), targets.size());

  std::array<bool, 2> outputReady{};
  for (size_t index = 0; index < outputs.size(); ++index) {
    const bool synced = outputs[index].sync();
    const bool closed = outputs[index].close();
    outputReady[index] = (converted & (1U << index)) != 0 && synced && closed &&
                         validateCachedThumbnail(stagingPaths[index].c_str(), &validation[index]);
  }
  const bool inputClosed = source.file.close();
  if (!retainCoverSource) clearCoverSource();

  if (!inputClosed || !sourceStillMatchesSnapshot()) {
    for (const auto& path : stagingPaths) Storage.remove(path.c_str());
    result.shared = ThumbnailStatus::IoError;
    result.carousel = ThumbnailStatus::IoError;
    return true;
  }

  std::array<bool, 2> published{};
  std::array<bool, 2> failed{};
  for (size_t index = 0; index < outputs.size(); ++index) {
    const StagedFileTransaction::Status publishStatus =
        outputReady[index] ? StagedFileTransaction::publish(finalPaths[index].c_str(), stagingPaths[index].c_str(),
                                                            (finalPaths[index] + ".bak").c_str(),
                                                            validateCachedThumbnail, &validation[index])
                           : StagedFileTransaction::Status::InvalidStaging;
    const bool bitmapPublished = publishStatus == StagedFileTransaction::Status::Published;
    if (bitmapPublished && sourceStillMatchesSnapshot() &&
        publishThumbIdentity(identityPaths[index], sourceIdentitySnapshot)) {
      Storage.remove(noCoverPaths[index].c_str());
      Storage.remove((noCoverPaths[index] + ".identity").c_str());
      published[index] = true;
    } else {
      Storage.remove(stagingPaths[index].c_str());
      // A failed publish may already have restored the old final and proof.
      // Remove only a newly published bitmap whose proof could not be bound.
      if (bitmapPublished) {
        Storage.remove(finalPaths[index].c_str());
        Storage.remove(identityPaths[index].c_str());
      }
      failed[index] = true;
    }
  }

  if (!sourceStillMatchesSnapshot()) {
    for (size_t index = 0; index < published.size(); ++index) {
      if (published[index]) {
        Storage.remove(finalPaths[index].c_str());
        Storage.remove(identityPaths[index].c_str());
        published[index] = false;
      }
    }
    result.shared = ThumbnailStatus::IoError;
    result.carousel = ThumbnailStatus::IoError;
    return true;
  }

  result.shared =
      published[0] ? ThumbnailStatus::Ready : (failed[0] ? ThumbnailStatus::IoError : ThumbnailStatus::Missing);
  result.carousel =
      published[1] ? ThumbnailStatus::Ready : (failed[1] ? ThumbnailStatus::IoError : ThumbnailStatus::Missing);
  return true;
}

Epub::ThumbnailPreparationStatus Epub::beginThumbnailPreparation(const ThumbnailRequest& request) {
  cancelThumbnailPreparation();
  // Page rasters have reader-visible priority. The reader scheduler retries
  // optional cover work after the active image transaction has settled.
  if (imagePreparationActive()) return ThumbnailPreparationStatus::Error;
  const ThumbnailRequest requested = allThumbnailVariants(request);
  if (!requested.shared && !requested.carousel) return ThumbnailPreparationStatus::NotNeeded;

  const ThumbnailSetStatus available = ensureThumbnails(requested, ThumbnailMode::EmbeddedOnly);
  const auto settled = [](const ThumbnailStatus status) {
    return status == ThumbnailStatus::Ready || status == ThumbnailStatus::NoCover;
  };
  if ((!requested.shared || settled(available.shared)) && (!requested.carousel || settled(available.carousel))) {
    return ThumbnailPreparationStatus::Ready;
  }

  const BookMetadataCache::BookMetadata* metadata = nullptr;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) {
    metadata = &bookMetadataCache->coreMetadata;
  } else if (hasTransientMetadata) {
    metadata = &transientMetadata;
  } else {
    return ThumbnailPreparationStatus::NeedsCoreMetadata;
  }
  if (!metadata || metadata->coverItemHref.empty()) return ThumbnailPreparationStatus::NotNeeded;

  const bool jpeg = FsHelpers::hasJpgExtension(metadata->coverItemHref);
  const bool png = FsHelpers::hasPngExtension(metadata->coverItemHref);
  if (!jpeg && !png) return ThumbnailPreparationStatus::NotNeeded;

  const std::string entryPath = FsHelpers::normalisePath(metadata->coverItemHref);
  coverStreamJob.reset(new (std::nothrow) ZipStreamReadJob());
  if (!coverStreamJob) return ThumbnailPreparationStatus::Error;
  const ZipStreamReadJob::BeginStatus started =
      coverStreamJob->begin(filepath, entryPath.c_str(), coverStreamOutput, 4096, MAX_EXTRACTED_COVER_BYTES);
  if (started != ZipStreamReadJob::BeginStatus::Started) {
    coverStreamJob.reset();
    // ZIP STORE covers do not need a scratch extraction, but still need the
    // synchronous direct-range JPEG conversion that follows this preflight.
    return started == ZipStreamReadJob::BeginStatus::NotApplicable
               ? ThumbnailPreparationStatus::NeedsSynchronousGeneration
               : ThumbnailPreparationStatus::Error;
  }

  if (!setupCacheDir()) {
    coverStreamJob->cancel();
    coverStreamJob.reset();
    return ThumbnailPreparationStatus::Error;
  }
  coverSourcePath = getCachePath() + (jpeg ? "/.cover.jpg" : "/.cover.png");
  if ((Storage.exists(coverSourcePath.c_str()) && !Storage.remove(coverSourcePath.c_str())) ||
      !Storage.openFileForWrite("EBP", coverSourcePath, coverStreamOutput)) {
    coverStreamJob->cancel();
    coverStreamJob.reset();
    clearCoverSource();
    return ThumbnailPreparationStatus::Error;
  }
  return ThumbnailPreparationStatus::InProgress;
}

Epub::ThumbnailPreparationStatus Epub::stepThumbnailPreparation() {
  if (!coverStreamJob) return ThumbnailPreparationStatus::Error;
  const ZipStreamReadJob::StepStatus status = coverStreamJob->step();
  if (status == ZipStreamReadJob::StepStatus::InProgress) return ThumbnailPreparationStatus::InProgress;

  coverStreamJob.reset();
  if (status != ZipStreamReadJob::StepStatus::Done) {
    coverStreamOutput.close();
    clearCoverSource();
    return ThumbnailPreparationStatus::Error;
  }

  const bool synced = coverStreamOutput.sync();
  const bool closed = coverStreamOutput.close();
  if (!synced || !closed || !sourceStillMatchesSnapshot()) {
    clearCoverSource();
    return ThumbnailPreparationStatus::Error;
  }
  return ThumbnailPreparationStatus::Ready;
}

void Epub::cancelThumbnailPreparation() {
  if (coverStreamJob) {
    coverStreamJob->cancel();
    coverStreamJob.reset();
  }
  if (coverStreamOutput) coverStreamOutput.close();
  clearCoverSource();
}

Epub::ThumbnailSetStatus Epub::ensureThumbnails(const ThumbnailRequest& request, const ThumbnailMode mode) {
  const ThumbnailRequest requested = allThumbnailVariants(request);
  ThumbnailSetStatus result;
  if (!requested.shared && !requested.carousel) return result;
  if (!ensureSourceIdentitySnapshot()) {
    if (requested.shared) result.shared = ThumbnailStatus::IoError;
    if (requested.carousel) result.carousel = ThumbnailStatus::IoError;
    return result;
  }
  const bool previousRetention = retainCoverSource;
  retainCoverSource = true;
  const int carouselWidth = requested.x3 ? CAROUSEL_THUMB_WIDTH : CAROUSEL_X4_THUMB_WIDTH;
  const int carouselHeight = requested.x3 ? CAROUSEL_THUMB_HEIGHT : CAROUSEL_X4_THUMB_HEIGHT;
  const auto settled = [](const ThumbnailStatus status) {
    return status == ThumbnailStatus::Ready || status == ThumbnailStatus::NoCover;
  };

  if (mode == ThumbnailMode::EmbeddedThenCover && requested.shared && requested.carousel) {
    result.shared = ensureSharedThumbnail(ThumbnailMode::EmbeddedOnly);
    result.carousel = ensureCarouselThumbnail(carouselWidth, carouselHeight, ThumbnailMode::EmbeddedOnly);
    const bool needsShared = !settled(result.shared);
    const bool needsCarousel = !settled(result.carousel);
    bool pairHandled = false;
    if (needsShared && needsCarousel) pairHandled = generateJpegThumbnailPair(carouselWidth, carouselHeight, result);
    if (!pairHandled) {
      if (needsShared) result.shared = ensureSharedThumbnail(mode);
      if (needsCarousel) result.carousel = ensureCarouselThumbnail(carouselWidth, carouselHeight, mode);
    }
  } else {
    if (requested.shared) result.shared = ensureSharedThumbnail(mode);
    if (requested.carousel) result.carousel = ensureCarouselThumbnail(carouselWidth, carouselHeight, mode);
  }
  retainCoverSource = previousRetention;
  if (!retainCoverSource) clearCoverSource();
  return result;
}

Epub::ThumbnailStatus Epub::ensureThumbnail(const int width, const int height, const char* embeddedEntry,
                                            const ThumbnailMode mode, const bool fitWithin) {
  if (width <= 0 || height <= 0 || !embeddedEntry) return ThumbnailStatus::Invalid;
  ThumbnailValidationContext validation{width, height, thumbnailFileSize(width, height), fitWithin};
  const std::string finalPath = getThumbBmpPath(height);
  const std::string noCoverPath = finalPath + ".nocover";
  const std::string identityPath = finalPath + (fitWithin ? ".fit-v2.identity" : ".identity");
  const std::string noCoverIdentityPath = noCoverPath + ".identity";

  if (!ensureSourceIdentitySnapshot()) return ThumbnailStatus::IoError;
  const ZipFile::SourceIdentity sourceIdentity = sourceIdentitySnapshot;
  ZipFile sourceFile(filepath);

  const bool finalValid = validateCachedThumbnail(finalPath.c_str(), &validation);
  ZipFile::SourceIdentity proof;
  if (finalValid && readThumbIdentity(identityPath.c_str(), proof) && proof == sourceIdentity) {
    return ThumbnailStatus::Ready;
  }
  if (validateNoCoverMarker(noCoverPath.c_str(), nullptr) && readThumbIdentity(noCoverIdentityPath.c_str(), proof) &&
      proof == sourceIdentity) {
    return ThumbnailStatus::NoCover;
  }

  // Legacy caches produced by the reader already have a durable source sidecar;
  // migrate their proof once instead of decoding the cover again.
  if (!fitWithin && finalValid && inspectSourceBinding() == SourceBindingStatus::Match) {
    if (publishThumbIdentity(identityPath, sourceIdentity)) return ThumbnailStatus::Ready;
  }
  if (!fitWithin && validateNoCoverMarker(noCoverPath.c_str(), nullptr) &&
      inspectSourceBinding() == SourceBindingStatus::Match) {
    if (publishThumbIdentity(noCoverIdentityPath, sourceIdentity)) return ThumbnailStatus::NoCover;
  }

  ZipFile::FileStatSlim stat{};
  const bool embeddedPresent = sourceFile.getFileStat(embeddedEntry, &stat);
  // Web-optimized EPUBs preserve the cover's aspect ratio, so their embedded
  // BMP dimensions may differ from the nominal frame. Bound extraction here;
  // validate the actual dimensions and payload with the production Bitmap
  // parser after copying.
  const bool embeddedValid = embeddedPresent && stat.method == ZIP_METHOD_STORED_LOCAL &&
                             stat.uncompressedSize >= 62U && stat.uncompressedSize <= 64U * 1024U;
  if (embeddedPresent && !embeddedValid && mode == ThumbnailMode::EmbeddedOnly) return ThumbnailStatus::Invalid;
  if (embeddedValid) {
    if (!setupCacheDir()) return ThumbnailStatus::IoError;
    const std::string stagingPath = finalPath + ".tmp";
    const std::string backupPath = finalPath + ".bak";
    const std::string sourceProof = identityPath + ".tmp";
    if ((Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) ||
        (Storage.exists(sourceProof.c_str()) && !Storage.remove(sourceProof.c_str()))) {
      return ThumbnailStatus::IoError;
    }
    HalFile output;
    if (!Storage.openFileForWrite("EBP", stagingPath, output)) return ThumbnailStatus::IoError;
    const bool copied = readItemContentsToStream(embeddedEntry, output, 1024);
    const bool synced = output.sync();
    const bool closed = output.close();
    if (!copied || !synced || !closed || !validateCachedThumbnail(stagingPath.c_str(), &validation) ||
        !sourceStillMatchesSnapshot()) {
      Storage.remove(stagingPath.c_str());
      return copied ? ThumbnailStatus::Invalid : ThumbnailStatus::IoError;
    }
    if (StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                       validateCachedThumbnail,
                                       &validation) != StagedFileTransaction::Status::Published) {
      Storage.remove(stagingPath.c_str());
      return ThumbnailStatus::IoError;
    }
    if (!sourceStillMatchesSnapshot()) {
      Storage.remove(finalPath.c_str());
      return ThumbnailStatus::IoError;
    }
    if (!publishThumbIdentity(identityPath, sourceIdentity)) {
      LOG_ERR("EBP", "Thumbnail proof publish failed; it will be regenerated next time");
    }
    Storage.remove(noCoverPath.c_str());
    Storage.remove(noCoverIdentityPath.c_str());
    return ThumbnailStatus::Ready;
  }

  if (mode == ThumbnailMode::EmbeddedOnly) return embeddedPresent ? ThumbnailStatus::Invalid : ThumbnailStatus::Missing;

  // Direct-SD EPUBs retain the existing converter path. Each UI variant gets
  // its own bounded derived thumbnail and source-identity proof.
  // A valid bitmap without matching proof belongs to an older source (or an
  // interrupted legacy cache); never let generateThumbBmp() accept it as
  // current merely because its pixels parse successfully.
  if ((fitWithin ? Storage.exists(finalPath.c_str()) : finalValid) && !Storage.remove(finalPath.c_str())) {
    return ThumbnailStatus::IoError;
  }
  if (validateNoCoverMarker(noCoverPath.c_str(), nullptr) && !Storage.remove(noCoverPath.c_str())) {
    return ThumbnailStatus::IoError;
  }
  Storage.remove(identityPath.c_str());
  Storage.remove(noCoverIdentityPath.c_str());
  BookMetadataCache::BookMetadata metadata;
  if ((!bookMetadataCache || !bookMetadataCache->isLoaded()) && !hasTransientMetadata) {
    if (!readCoreMetadata(metadata)) return ThumbnailStatus::IoError;
  }
  if (generateThumbBmp(width, height, !fitWithin) && validateCachedThumbnail(finalPath.c_str(), &validation)) {
    if (!sourceStillMatchesSnapshot()) {
      Storage.remove(finalPath.c_str());
      return ThumbnailStatus::IoError;
    }
    if (!publishThumbIdentity(identityPath, sourceIdentity)) {
      LOG_ERR("EBP", "Converted thumbnail proof publish failed");
    }
    if (!sourceStillMatchesSnapshot()) {
      Storage.remove(finalPath.c_str());
      Storage.remove(identityPath.c_str());
      return ThumbnailStatus::IoError;
    }
    return ThumbnailStatus::Ready;
  }
  if (!sourceStillMatchesSnapshot()) return ThumbnailStatus::IoError;
  if (validateNoCoverMarker(noCoverPath.c_str(), nullptr) &&
      publishThumbIdentity(noCoverIdentityPath, sourceIdentity)) {
    return ThumbnailStatus::NoCover;
  }
  return embeddedPresent ? ThumbnailStatus::Invalid : ThumbnailStatus::Missing;
}

bool Epub::generateThumbBmp(const int height) const {
  return generateThumbBmp(static_cast<int>(height * 0.6f), height, true);
}

bool Epub::generateThumbBmp(const int width, const int height, const bool crop) const {
  if (width <= 0 || height <= 0) return false;
  const std::string finalPath = getThumbBmpPath(height);
  const std::string stagingPath = finalPath + ".tmp";
  const std::string noCoverPath = finalPath + ".nocover";
  const BitmapCacheState cacheState = Bitmap::inspectDerivedCache(finalPath);
  if (cacheState == BitmapCacheState::Ready) return true;
  if (cacheState == BitmapCacheState::IoError) return false;
  if (validateNoCoverMarker(noCoverPath.c_str(), nullptr)) return false;

  const BookMetadataCache::BookMetadata* metadata = nullptr;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) {
    metadata = &bookMetadataCache->coreMetadata;
  } else if (hasTransientMetadata) {
    metadata = &transientMetadata;
  }
  if (!metadata) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  if (!setupCacheDir()) return false;
  const auto coverImageHref = metadata->coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
    if (coverResolutionComplete) writeNoCoverMarker(noCoverPath);
    return false;
  }

  const bool jpeg = FsHelpers::hasJpgExtension(coverImageHref);
  const bool png = FsHelpers::hasPngExtension(coverImageHref);
  if (!jpeg && !png) {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
    writeNoCoverMarker(noCoverPath);
    return false;
  }

  LOG_DBG("EBP", "Generating thumb BMP from %s cover image", jpeg ? "JPG" : "PNG");
  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;

  CoverSource source;
  if (!openCoverSource(coverImageHref, jpeg, source)) return false;

  HalFile output;
  if (!Storage.openFileForWrite("EBP", stagingPath, output)) {
    source.file.close();
    if (!retainCoverSource) clearCoverSource();
    return false;
  }
  const bool converted =
      jpeg ? (source.ranged
                  ? JpegToBmpConverter::jpegFileRangeTo1BitBmpStreamWithSize(source.file, source.offset, source.length,
                                                                             output, width, height, crop)
                  : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source.file, output, width, height, crop))
           : PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source.file, output, width, height, crop);
  const bool outputSynced = output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = source.file.close();
  if (!retainCoverSource) clearCoverSource();
  if (!converted || !outputSynced || !outputClosed || !inputClosed || !publishBitmap(finalPath, stagingPath)) {
    Storage.remove(stagingPath.c_str());
    LOG_ERR("EBP", "Failed to generate thumbnail from %s cover image", jpeg ? "JPG" : "PNG");
    return false;
  }
  Storage.remove(noCoverPath.c_str());
  LOG_DBG("EBP", "Generated thumbnail from %s cover image", jpeg ? "JPG" : "PNG");
  return true;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop, const size_t maxOutputSize,
                                    bool* const outputLimitExceeded) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop, maxOutputSize,
                                            outputLimitExceeded);
}

bool Epub::extractItemToFileAtomically(const std::string& itemHref, const std::string& finalPath) const {
  const std::string_view finalPathView(finalPath);
  if (itemHref.empty() || finalPath.empty() ||
      (!FsHelpers::hasJpgExtension(finalPathView) && !FsHelpers::hasPngExtension(finalPathView))) {
    return false;
  }
  const std::string stagingPath = finalPath + ".tmp";
  const std::string backupPath = finalPath + ".bak";
  const std::string markerPath = finalPath + ".pending";
  if (!reconcileImagePublication(finalPath, stagingPath, backupPath, markerPath)) return false;
  if (validateRasterFile(finalPath.c_str(), nullptr)) return sourceStillMatchesSnapshot();
  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;

  HalFile output;
  if (!Storage.openFileForWrite("EBP", stagingPath, output)) return false;
  constexpr size_t MAX_EXTRACTED_RASTER_BYTES = 16U * 1024U * 1024U;
  const bool extracted = readItemContentsToStream(itemHref, output, 4096, false, MAX_EXTRACTED_RASTER_BYTES);
  const bool synced = output.sync();
  const bool closed = output.close();
  if (!extracted || !synced || !closed || !sourceStillMatchesSnapshot() ||
      !validateRasterFile(stagingPath.c_str(), nullptr)) {
    Storage.remove(stagingPath.c_str());
    return false;
  }

  if (!writeImagePublishMarker(markerPath)) return false;
  const auto published =
      StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(), validateRasterFile);
  if (published != StagedFileTransaction::Status::Published) {
    reconcileImagePublication(finalPath, stagingPath, backupPath, markerPath);
    return false;
  }
  return Storage.remove(markerPath.c_str());
}

Epub::ImagePreparationStatus Epub::beginImagePreparation(const std::string& itemHref, const std::string& finalPath) {
  if (imagePreparationActive()) cancelImagePreparation();
  const std::string_view finalPathView(finalPath);
  if (itemHref.empty() || finalPath.empty() ||
      (!FsHelpers::hasJpgExtension(finalPathView) && !FsHelpers::hasPngExtension(finalPathView))) {
    return ImagePreparationStatus::Error;
  }
  // A page image is useful to the active reading view; an optional cover is
  // not. Cancelling here also enforces that the two inflater jobs never share
  // the constrained heap even if a caller bypasses the reader scheduler.
  if (coverStreamJob) cancelThumbnailPreparation();
  const std::string backupPath = finalPath + ".bak";
  const std::string stagingPath = finalPath + ".tmp";
  const std::string markerPath = finalPath + ".pending";
  if (!reconcileImagePublication(finalPath, stagingPath, backupPath, markerPath)) {
    return ImagePreparationStatus::Error;
  }
  if (validateRasterFile(finalPath.c_str(), nullptr)) {
    return sourceStillMatchesSnapshot() ? ImagePreparationStatus::NotNeeded : ImagePreparationStatus::Error;
  }

  imageStreamFinalPath = finalPath;
  imageStreamStagingPath = stagingPath;
  imagePublishMarkerPath = markerPath;
  if (Storage.exists(imageStreamStagingPath.c_str()) && !Storage.remove(imageStreamStagingPath.c_str())) {
    imageStreamFinalPath.clear();
    imageStreamStagingPath.clear();
    return ImagePreparationStatus::Error;
  }
  if (!Storage.openFileForWrite("EBP", imageStreamStagingPath, imageStreamOutput)) {
    imageStreamFinalPath.clear();
    imageStreamStagingPath.clear();
    return ImagePreparationStatus::Error;
  }

  imageDigestingOutput =
      std::unique_ptr<ImageDigestingOutput>(new (std::nothrow) ImageDigestingOutput(imageStreamOutput));
  imageStreamJob = std::unique_ptr<ZipStreamReadJob>(new (std::nothrow) ZipStreamReadJob());
  constexpr size_t MAX_EXTRACTED_RASTER_BYTES = 16U * 1024U * 1024U;
  const std::string sourcePath = FsHelpers::normalisePath(itemHref);
  if (!imageDigestingOutput || !imageStreamJob ||
      imageStreamJob->beginCooperativeLookup(filepath, sourcePath.c_str(), *imageDigestingOutput, 4096,
                                             MAX_EXTRACTED_RASTER_BYTES) != ZipStreamReadJob::BeginStatus::Started) {
    cancelImagePreparation();
    return ImagePreparationStatus::Error;
  }
  return ImagePreparationStatus::InProgress;
}

Epub::ImagePreparationStatus Epub::stepImagePreparation() {
  if (imageStreamJob) {
    const ZipStreamReadJob::StepStatus status = imageStreamJob->step();
    if (status == ZipStreamReadJob::StepStatus::InProgress) return ImagePreparationStatus::InProgress;

    imageStreamJob.reset();
    const bool synced = status == ZipStreamReadJob::StepStatus::Done && imageStreamOutput.sync();
    const bool closed = imageStreamOutput.close();
    if (!synced || !closed || !imageDigestingOutput || !imageDigestingOutput->validRaster()) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }

    if (!writeImagePublishMarker(imagePublishMarkerPath)) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }
    imagePublishPending = true;
    const std::string backupPath = imageStreamFinalPath + ".bak";
    const auto published = StagedFileTransaction::beginPendingPublish(
        imageStreamFinalPath.c_str(), imageStreamStagingPath.c_str(), backupPath.c_str(),
        imageDigestingOutput->digest().size, validateRasterFile);
    if (published != StagedFileTransaction::Status::Published) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }
    imagePublishedDigestJob = std::unique_ptr<ImageDigestReadJob>(new (std::nothrow) ImageDigestReadJob());
    if (!imagePublishedDigestJob || !imagePublishedDigestJob->begin(imageStreamFinalPath)) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }
    return ImagePreparationStatus::InProgress;
  }

  if (imagePublishedDigestJob) {
    if (!imageDigestingOutput || !imagePublishPending) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }
    const auto digestStatus = imagePublishedDigestJob->step(4096);
    if (digestStatus == ImageDigestReadJob::StepStatus::InProgress) return ImagePreparationStatus::InProgress;
    if (digestStatus != ImageDigestReadJob::StepStatus::Done ||
        !(imagePublishedDigestJob->digest() == imageDigestingOutput->digest())) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }

    imagePublishedDigestJob.reset();
    imageSourceIdentityJob = std::unique_ptr<ZipSourceIdentityJob>(new (std::nothrow) ZipSourceIdentityJob());
    if (!imageSourceIdentityJob || !imageSourceIdentityJob->begin(filepath)) {
      cancelImagePreparation();
      return ImagePreparationStatus::Error;
    }
    return ImagePreparationStatus::InProgress;
  }

  if (!imageSourceIdentityJob || !imageDigestingOutput || !imagePublishPending) {
    cancelImagePreparation();
    return ImagePreparationStatus::Error;
  }
  ZipFile::SourceIdentity currentIdentity;
  ZipSourceIdentityJob::FileStamp currentStamp;
  const auto identityStatus = imageSourceIdentityJob->step(4096, currentIdentity, &currentStamp);
  if (identityStatus == ZipSourceIdentityJob::StepStatus::InProgress) return ImagePreparationStatus::InProgress;
  imageSourceIdentityJob.reset();
  if (identityStatus != ZipSourceIdentityJob::StepStatus::Done || currentIdentity != sourceIdentitySnapshot ||
      !sourcePathMatchesIdentityJob(filepath.c_str(), currentIdentity, currentStamp)) {
    cancelImagePreparation();
    return ImagePreparationStatus::Error;
  }

  // Keep the marker visible until the rollback candidate is gone. A failure or
  // reset before marker removal therefore either restores the old raster or
  // discards the new derived cache; readers never observe an ambiguous .bak.
  const std::string backupPath = imageStreamFinalPath + ".bak";
  if (!StagedFileTransaction::commitPendingPublish(backupPath.c_str())) {
    cancelImagePreparation();
    return ImagePreparationStatus::Error;
  }
  if (!Storage.remove(imagePublishMarkerPath.c_str())) {
    cancelImagePreparation();
    return ImagePreparationStatus::Error;
  }
  imagePublishPending = false;
  cancelImagePreparation();
  return ImagePreparationStatus::Ready;
}

bool Epub::deferImagePreparationCleanup() {
  // Only an unpublished stream is safe to abandon without synchronous FAT
  // rollback. Once publication starts, cancelImagePreparation() must retain
  // the marker/backup recovery contract.
  if (!imageStreamJob || imagePublishPending || imageSourceIdentityJob || imagePublishedDigestJob ||
      !imageDeferredCleanupPath.empty()) {
    return false;
  }

  imageStreamJob->cancel();
  imageStreamJob.reset();
  if (imageStreamOutput) imageStreamOutput.close();
  imageDeferredCleanupPath = std::move(imageStreamStagingPath);
  imageDigestingOutput.reset();
  imageStreamFinalPath.clear();
  imagePublishMarkerPath.clear();
  return true;
}

void Epub::cancelImagePreparation() {
  if (imageStreamJob) imageStreamJob->cancel();
  imageStreamJob.reset();
  if (imageSourceIdentityJob) imageSourceIdentityJob->cancel();
  imageSourceIdentityJob.reset();
  imagePublishedDigestJob.reset();
  if (imageStreamOutput) imageStreamOutput.close();
  bool removePublishMarker = true;
  if (imagePublishPending) {
    const std::string backupPath = imageStreamFinalPath + ".bak";
    if (!StagedFileTransaction::rollbackPendingPublish(imageStreamFinalPath.c_str(), backupPath.c_str())) {
      LOG_ERR("EBP", "Could not roll back pending image-cache publication: %s", imageStreamFinalPath.c_str());
      removePublishMarker = false;
    }
    imagePublishPending = false;
  }
  if (!imageStreamStagingPath.empty() && Storage.exists(imageStreamStagingPath.c_str())) {
    Storage.remove(imageStreamStagingPath.c_str());
  }
  if (!imageDeferredCleanupPath.empty() && Storage.exists(imageDeferredCleanupPath.c_str())) {
    if (!Storage.remove(imageDeferredCleanupPath.c_str())) {
      LOG_ERR("EBP", "Could not remove deferred image-cache scratch: %s", imageDeferredCleanupPath.c_str());
    }
  }
  if (removePublishMarker && !imagePublishMarkerPath.empty() && Storage.exists(imagePublishMarkerPath.c_str())) {
    Storage.remove(imagePublishMarkerPath.c_str());
  }
  imageDigestingOutput.reset();
  imageStreamFinalPath.clear();
  imageStreamStagingPath.clear();
  imagePublishMarkerPath.clear();
  imageDeferredCleanupPath.clear();
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return 0;
  return bookMetadataCache->getSpineCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

int Epub::getAdjacentLinearSpineIndex(const int spineIndex, const bool forward) const {
  const int spineCount = getSpineItemsCount();
  const int exhausted = forward ? spineCount : -1;
  if (spineCount <= 0) return exhausted;

  for (int candidate = spineIndex + (forward ? 1 : -1); candidate >= 0 && candidate < spineCount;
       candidate += forward ? 1 : -1) {
    if (getSpineItem(candidate).linear) return candidate;
  }
  return exhausted;
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;
  return bookMetadataCache->getSpineTocIndex(spineIndex);
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // With no explicit start target, begin at the first primary reading-order
    // item and leave linear="no" auxiliaries reachable only through links/TOC.
    const int firstLinear = getAdjacentLinearSpineIndex(-1, true);
    return firstLinear < getSpineItemsCount() ? firstLinear : 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  const int firstLinear = getAdjacentLinearSpineIndex(-1, true);
  return firstLinear < getSpineItemsCount() ? firstLinear : 0;
}

// Calculate progress in book (returns 0.0-1.0)
bool Epub::calculateProgressChecked(const int currentSpineIndex, const float currentSpineRead, float& progress) const {
  progress = 0.0F;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || !std::isfinite(currentSpineRead)) return false;

  const int spineCount = getSpineItemsCount();
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) return false;

  // This is the authoritative variant used outside the active reader. Re-read
  // the entries so a removed/corrupt book.bin cannot be turned into a
  // plausible percentage merely because the in-session numeric cache exists.
  const size_t bookSize = getSpineItem(spineCount - 1).cumulativeSize;
  if (!bookMetadataCache->isLoaded() || bookSize == 0) return false;

  const size_t previousSize =
      currentSpineIndex > 0 ? getSpineItem(currentSpineIndex - 1).cumulativeSize : static_cast<size_t>(0);
  if (!bookMetadataCache->isLoaded()) return false;
  const size_t currentSize =
      currentSpineIndex == spineCount - 1 ? bookSize : getSpineItem(currentSpineIndex).cumulativeSize;
  if (!bookMetadataCache->isLoaded() || currentSize < previousSize || currentSize > bookSize) return false;

  const float chapterProgress = std::clamp(currentSpineRead, 0.0F, 1.0F);
  const float chapterSize = static_cast<float>(currentSize - previousSize);
  const float calculated =
      (static_cast<float>(previousSize) + chapterProgress * chapterSize) / static_cast<float>(bookSize);
  if (!std::isfinite(calculated)) return false;
  progress = std::clamp(calculated, 0.0F, 1.0F);
  return true;
}

float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || !std::isfinite(currentSpineRead)) return 0.0F;
  const int spineCount = getSpineItemsCount();
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) return 0.0F;
  const size_t bookSize = getCumulativeSpineItemSize(spineCount - 1);
  if (bookSize == 0) return 0.0F;
  const size_t previousSize = currentSpineIndex > 0 ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t currentSize =
      currentSpineIndex == spineCount - 1 ? bookSize : getCumulativeSpineItemSize(currentSpineIndex);
  if (currentSize < previousSize || currentSize > bookSize) return 0.0F;
  const float chapterProgress = std::clamp(currentSpineRead, 0.0F, 1.0F);
  const float calculated =
      (static_cast<float>(previousSize) + chapterProgress * static_cast<float>(currentSize - previousSize)) /
      static_cast<float>(bookSize);
  return std::isfinite(calculated) ? std::clamp(calculated, 0.0F, 1.0F) : 0.0F;
}

int Epub::resolveHrefToSpineIndex(const std::string& href, const int sourceSpineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  std::string decodedTarget = FsHelpers::decodeUriEscapes(rawTarget);
  if (!decodedTarget.empty() && decodedTarget.front() != '/' && sourceSpineIndex >= 0 &&
      sourceSpineIndex < getSpineItemsCount()) {
    const std::string sourceHref = getSpineItem(sourceSpineIndex).href;
    const size_t sourceSlash = sourceHref.find_last_of('/');
    if (sourceSlash != std::string::npos) decodedTarget.insert(0, sourceHref.substr(0, sourceSlash + 1));
  }
  const std::string target = FsHelpers::normalisePath(decodedTarget);

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  // Prefer an exact path match across the complete spine. A matching basename
  // earlier in the spine must not hide an exact path that appears later.
  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    if (spineHref == target) return i;
  }

  int filenameMatch = -1;
  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Retain the legacy filename fallback only when it is unambiguous.
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) {
      if (filenameMatch >= 0) return -1;
      filenameMatch = i;
    }
  }
  return filenameMatch;
}
