#pragma once

#include <Print.h>
#include <ZipFile.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // False when ZIP enumeration stopped before the complete stylesheet set
  // could be established. A partial set must never be published as valid.
  bool cssDiscoveryComplete = true;
  // One central-directory scan is shared by preflight, sidecar binding, and
  // book.bin validation. A separate forced scan at the end detects a source
  // that changed while the EPUB was being opened or indexed.
  mutable ZipFile::SourceIdentity sourceIdentitySnapshot{};
  mutable bool hasSourceIdentitySnapshot = false;
  bool externalCssUnavailable = false;
  BookMetadataCache::BookMetadata transientMetadata;
  bool hasTransientMetadata = false;
  // True only when OPF metadata, or a completely inspected guide wrapper,
  // established the supported cover outcome. Transient read failures must
  // not create a permanent no-cover marker.
  bool coverResolutionComplete = false;
  // A bounded thumbnail batch reopens this one extracted raster sequentially.
  // The JPEG/PNG decoders stay single-output and only one decoder buffer lives
  // at a time; the expensive ZIP extraction is not repeated for every size.
  mutable bool retainCoverSource = false;
  mutable std::string coverSourcePath;
  std::unique_ptr<ZipStreamReadJob> coverStreamJob;
  HalFile coverStreamOutput;

  struct CoverSource {
    HalFile file;
    uint64_t offset = 0;
    uint32_t length = 0;
    bool ranged = false;
  };

  bool ensureSourceIdentitySnapshot() const;
  bool sourceStillMatchesSnapshot() const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  bool parseCssFiles() const;
  bool prepareCssCache(bool verifySourceAtEntry);
  bool openCoverSource(const std::string& coverImageHref, bool jpeg, CoverSource& source) const;
  void clearCoverSource() const;
  bool generateThumbBmp(int width, int height, bool crop) const;

 public:
  enum class SourceBindingStatus : uint8_t { Match, Missing, Mismatch, NewerVersion, Invalid, IoError };
  enum class ThumbnailMode : uint8_t { EmbeddedOnly, EmbeddedThenCover };
  enum class ThumbnailStatus : uint8_t { Ready, NoCover, Missing, Invalid, IoError };
  enum class ThumbnailPreparationStatus : uint8_t {
    NotNeeded,
    InProgress,
    NeedsSynchronousGeneration,
    Ready,
    Error,
  };
  struct ThumbnailRequest {
    bool shared = false;
    bool carousel = false;
    bool x3 = false;
  };
  struct ThumbnailSetStatus {
    ThumbnailStatus shared = ThumbnailStatus::Missing;
    ThumbnailStatus carousel = ThumbnailStatus::Missing;
  };

  static constexpr int SHARED_THUMB_WIDTH = 144;
  static constexpr int SHARED_THUMB_HEIGHT = 240;
  static constexpr int CAROUSEL_THUMB_WIDTH = 273;
  static constexpr int CAROUSEL_THUMB_HEIGHT = 456;
  static constexpr int CAROUSEL_X4_THUMB_WIDTH = 249;
  static constexpr int CAROUSEL_X4_THUMB_HEIGHT = 415;
  static const char* sharedThumbnailEntry();
  static const char* carouselThumbnailEntry(int width, int height);

  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() {
    cancelThumbnailPreparation();
    clearCoverSource();
  }
  std::string& getBasePath() { return contentBasePath; }
  // Read-only preflight used before loading path-keyed user state. A source
  // mismatch means a different EPUB now occupies this path.
  BookMetadataCache::LoadStatus inspectCache();
  // Stream only container.xml and content.opf for library presentation. This
  // does not build spine/TOC/CSS/page caches or bind path-keyed user state.
  bool readCoreMetadata(BookMetadataCache::BookMetadata& metadata);
  SourceBindingStatus inspectSourceBinding() const;
  bool bindCurrentSource() const;
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  // Ensure the external stylesheet cache exists and passes a complete read-back.
  // Safe to call repeatedly; already-valid caches leave section caches untouched.
  bool ensureCssCache();
  bool isExternalCssUnavailable() const { return externalCssUnavailable; }
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  const std::string& getCoverItemHref() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  // Generate a sleep cover and every currently requested UI thumbnail from
  // the same extracted JPG/PNG scratch file. Thumbnail failures do not hide a
  // successfully generated sleep cover.
  bool generateCoverBmp(bool cropped, const ThumbnailRequest& thumbnails);
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  // Materialize the optimized EPUB thumbnail once at the canonical size. The
  // fallback mode retains the existing JPG/PNG converter for direct-SD books.
  ThumbnailStatus ensureSharedThumbnail(ThumbnailMode mode = ThumbnailMode::EmbeddedThenCover);
  // Materialize the larger Home carousel cover without replacing the shared
  // library thumbnail.
  ThumbnailStatus ensureCarouselThumbnail(int width, int height, ThumbnailMode mode = ThumbnailMode::EmbeddedThenCover);
  // Once a UI requests either derived thumbnail, materialize both canonical
  // variants for this device so changing between Library covers and Home style
  // 4 never requires extracting the EPUB cover again.
  ThumbnailSetStatus ensureThumbnails(const ThumbnailRequest& request,
                                      ThumbnailMode mode = ThumbnailMode::EmbeddedThenCover);
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                size_t maxOutputSize = SIZE_MAX, bool* outputLimitExceeded = nullptr) const;
  // Deflated cover rasters are extracted one bounded chunk per main-loop
  // iteration. ZIP STORE covers instead report NeedsSynchronousGeneration so
  // callers can present progress before direct-range conversion.
  ThumbnailPreparationStatus beginThumbnailPreparation(const ThumbnailRequest& request);
  ThumbnailPreparationStatus stepThumbnailPreparation();
  void cancelThumbnailPreparation();
  bool thumbnailPreparationActive() const { return coverStreamJob != nullptr; }
  // Extract a supported raster item into the derived cache without exposing a
  // partially written final file. Existing valid output is reused.
  bool extractItemToFileAtomically(const std::string& itemHref, const std::string& finalPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  // Returns false if book.bin becomes unavailable or internally inconsistent
  // while progress is being calculated. Callers that display authoritative
  // state should not turn that failure into a plausible 0% value.
  bool calculateProgressChecked(int currentSpineIndex, float currentSpineRead, float& progress) const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

 private:
  static constexpr ThumbnailRequest allThumbnailVariants(ThumbnailRequest request) {
    if (request.shared || request.carousel) request.shared = request.carousel = true;
    return request;
  }
  bool generateJpegThumbnailPair(int carouselWidth, int carouselHeight, ThumbnailSetStatus& result);
  ThumbnailStatus ensureThumbnail(int width, int height, const char* embeddedEntry, ThumbnailMode mode, bool fitWithin);
};
