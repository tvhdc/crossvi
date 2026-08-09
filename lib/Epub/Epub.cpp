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
  if (!path || !Storage.exists(path)) return false;
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
  if (!path || !context || context->width <= 0 || context->height <= 0 || !Storage.exists(path)) return false;
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
  if (!path || !Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return false;
  std::string imagePath(path);
  if ((imagePath.size() >= 4 && (imagePath.compare(imagePath.size() - 4, 4, ".tmp") == 0 ||
                                 imagePath.compare(imagePath.size() - 4, 4, ".bak") == 0))) {
    imagePath.resize(imagePath.size() - 4);
  }
  const std::string_view imagePathView(imagePath);
  if (!FsHelpers::hasJpgExtension(imagePathView) && !FsHelpers::hasPngExtension(imagePathView)) {
    file.close();
    return false;
  }

  ImageDimsProbe probe;
  std::array<uint8_t, 1024> buffer{};
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
}  // namespace

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

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  coverResolutionComplete = !bookMetadata.coverItemHref.empty() || opfParser.guideCoverPageHref.empty();

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
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
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG only; skip GIF so an unsupported wrapper image
            // does not block a later supported cover reference.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      // The guide wrapper was read completely. From this point an empty
      // imageRef is a verified "no supported cover" result, not a transient
      // ZIP/I/O failure, so thumbnail generation may persist its small marker.
      coverResolutionComplete = true;

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
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

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  const std::string ncxContentBasePath = tocNcxItem.substr(0, tocNcxItem.find_last_of('/') + 1);
  TocNcxParser ncxParser(ncxContentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
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

bool Epub::readCoreMetadata(BookMetadataCache::BookMetadata& metadata) {
  // Prefer the source-verified metadata cache: book.bin is bound to the exact
  // bytes of this EPUB, so a matching cache supplies the same core fields the
  // OPF parse would, without opening the zip or parsing XML. Falls back to a
  // full parse when the cache is absent, stale, or corrupt, and also when the
  // cached cover href is empty: that state can be a transient guide-page read
  // failure at index time rather than a verified no-cover result, and only the
  // parse (which sets coverResolutionComplete) can re-resolve it.
  bool fromCache = false;
  if (bookMetadataCache && bookMetadataCache->isLoaded()) {
    fromCache = !bookMetadataCache->coreMetadata.coverItemHref.empty();
  } else if (Storage.exists((cachePath + "/book.bin").c_str()) && ensureSourceIdentitySnapshot()) {
    bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
    const bool loaded = bookMetadataCache &&
                        bookMetadataCache->load(sourceIdentitySnapshot) == BookMetadataCache::LoadStatus::Loaded;
    if (!bookMetadataCache) LOG_ERR("EBP", "Not enough memory for cached metadata; parsing OPF instead");
    if (loaded) fromCache = !bookMetadataCache->coreMetadata.coverItemHref.empty();
  }
  if (fromCache) {
    metadata = bookMetadataCache->coreMetadata;
    // A non-empty cached cover is a resolved result (the guide-cover fallback
    // already ran when the cache was built).
    coverResolutionComplete = true;
  } else {
    // Drop the stale cache before the fallback parse: thumbnail consumers
    // prefer bookMetadataCache->coreMetadata over transientMetadata, and the
    // re-parse may have found a cover the old cache baked in as empty.
    bookMetadataCache.reset();
    metadata = {};
    if (!parseContentOpf(metadata, /*writeSpineEntries=*/false)) return false;
  }
  transientMetadata = metadata;
  hasTransientMetadata = true;
  return true;
}

bool Epub::prepareCssCache(const bool verifySourceAtEntry) {
  if (!cssParser || !bookMetadataCache || (verifySourceAtEntry && !sourceStillMatchesSnapshot())) {
    LOG_ERR("EBP", "Cannot prepare CSS cache without a loaded, matching EPUB");
    return false;
  }

  if (cssParser->loadFromCache()) {
    cssParser->clear();
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

  if (!saved || !metadataReloaded || !sourceStillMatchesSnapshot() || !cssParser->loadFromCache()) {
    LOG_ERR("EBP", "Failed to build and verify CSS cache");
    cssParser->clear();
    cssParser->deleteCache();
    return false;
  }

  cssParser->clear();
  externalCssUnavailable = false;
  return true;
}

bool Epub::ensureCssCache() { return prepareCssCache(true); }

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  EpubLoadDebugMetric debugLoadMetric;
#endif
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  externalCssUnavailable = false;

  // The durable sidecar survives a derived-cache clear. Refuse to load any
  // metadata or user state unless it still identifies this exact EPUB.
  if (inspectSourceBinding() != SourceBindingStatus::Match) return false;

  // Initialize spine/TOC cache
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!bookMetadataCache || !cssParser) {
    LOG_ERR("EBP", "Not enough memory to initialize EPUB metadata and CSS");
    bookMetadataCache.reset();
    cssParser.reset();
    return false;
  }
  const auto prepareCssForLoad = [this, skipLoadingCss]() {
    // load() already verified the source before reading derived data and
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
  };

  // Try to load existing cache first
  const BookMetadataCache::LoadStatus cacheStatus = bookMetadataCache->load(sourceIdentitySnapshot);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  debugLoadMetric.classifyCache(cacheStatus, buildIfMissing);
#endif
  if (cacheStatus == BookMetadataCache::LoadStatus::Loaded) {
    if (!prepareCssForLoad()) return false;
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    if (!sourceStillMatchesSnapshot()) {
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

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

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

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t indexingStart = static_cast<uint32_t>(millis());
#endif

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t opfStart = static_cast<uint32_t>(millis());
#endif
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("EBP", "OPF pass completed in %u ms", static_cast<unsigned>(static_cast<uint32_t>(millis()) - opfStart));
#endif

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t tocStart = static_cast<uint32_t>(millis());
#endif
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("EBP", "TOC pass completed in %u ms", static_cast<unsigned>(static_cast<uint32_t>(millis()) - tocStart));
#endif

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t buildStart = static_cast<uint32_t>(millis());
#endif
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata, sourceIdentitySnapshot)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  LOG_DBG("EBP", "buildBookBin completed in %u ms",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - buildStart));
  LOG_DBG("EBP", "Total indexing completed in %u ms",
          static_cast<unsigned>(static_cast<uint32_t>(millis()) - indexingStart));
#endif

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!prepareCssForLoad()) return false;

  // ensureCssCache() reloads book.bin after temporarily lending its memory to
  // CSS parsing. A style-free load still needs the normal first reload here.
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
    if (!bookMetadataCache) {
      LOG_ERR("EBP", "Not enough memory to reload EPUB cache after indexing");
      return false;
    }
  }
  if (!bookMetadataCache->isLoaded() &&
      bookMetadataCache->load(sourceIdentitySnapshot) != BookMetadataCache::LoadStatus::Loaded) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  if (!sourceStillMatchesSnapshot()) {
    LOG_ERR("EBP", "EPUB changed while indexing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  debugLoadMetric.markSuccess();
#endif
  return true;
}

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

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
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

  setupCacheDir();
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
    BookMetadataCache::BookMetadata loaded;
    if (!readCoreMetadata(loaded)) return ThumbnailPreparationStatus::Error;
    metadata = &transientMetadata;
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

  setupCacheDir();
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
    setupCacheDir();
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

  setupCacheDir();
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
  const auto recovered = StagedFileTransaction::recover(finalPath.c_str(), backupPath.c_str(), validateRasterFile);
  if (recovered == StagedFileTransaction::Status::IoError) return false;
  if (validateRasterFile(finalPath.c_str(), nullptr)) return true;
  if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) return false;

  HalFile output;
  if (!Storage.openFileForWrite("EBP", stagingPath, output)) return false;
  constexpr size_t MAX_EXTRACTED_RASTER_BYTES = 16U * 1024U * 1024U;
  const bool extracted = readItemContentsToStream(itemHref, output, 4096, false, MAX_EXTRACTED_RASTER_BYTES);
  const bool synced = output.sync();
  const bool closed = output.close();
  if (!extracted || !synced || !closed || !validateRasterFile(stagingPath.c_str(), nullptr)) {
    Storage.remove(stagingPath.c_str());
    return false;
  }

  const auto published =
      StagedFileTransaction::publish(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(), validateRasterFile);
  return published == StagedFileTransaction::Status::Published;
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
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
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
  return 0;
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

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
