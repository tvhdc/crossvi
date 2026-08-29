#include "BookMetadataCache.h"

#include <BufferedFile.h>
#include <Logging.h>
#include <Serialization.h>
#include <StagedFileTransaction.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <array>
#include <deque>
#include <limits>
#include <new>

#include "FsHelpers.h"
#include "SourceIdentityCodec.h"

namespace {
// v10 binds all derived metadata and section caches to the backing EPUB's ZIP
// central directory, preventing a different book at the same path from
// inheriting them.
// v11 stops treating ambiguous EPUB 2 guide type="text" entries as an
// authoritative reading start. Rebuild older caches so the corrected OPF
// interpretation reaches Epub::getTextReferenceHref().
// v12 retains the OPF itemref linear flag so sequential page turns can skip
// auxiliary content without making direct links to it unreachable.
// v13 rebuilds metadata parsed before OPF/DC namespaces were validated.
constexpr uint8_t BOOK_CACHE_VERSION = 13;
constexpr uint32_t BOOK_CACHE_COMMIT_MARKER = 0x424D434B;  // "BMCK"
constexpr char bookBinFile[] = "/book.bin";
constexpr char bookBinStagingFile[] = "/book.bin.tmp";
constexpr char bookBinBackupFile[] = "/book.bin.bak";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;
constexpr size_t BOOK_CACHE_FIXED_HEADER_SIZE = sizeof(BOOK_CACHE_VERSION) + sizeof(uint32_t) + sizeof(uint16_t) +
                                                sizeof(uint16_t) + SourceIdentityCodec::PAYLOAD_SIZE + sizeof(uint32_t);
constexpr size_t BOOK_CACHE_MIN_METADATA_SIZE = sizeof(uint32_t) * 5;
constexpr size_t BOOK_CACHE_MAX_METADATA_SIZE = 64 * 1024;
// No valid path/title/anchor entry should consume a material fraction of the
// ESP32-C3 heap. This also prevents a corrupt LUT from turning a bounded read
// into a large std::string allocation.
constexpr size_t BOOK_CACHE_MAX_ENTRY_SIZE = 32 * 1024;
constexpr size_t BOOK_CACHE_LUT_CHUNK_SIZE = 64;
constexpr size_t BOOK_CACHE_MIN_FILE_SIZE =
    BOOK_CACHE_FIXED_HEADER_SIZE + BOOK_CACHE_MIN_METADATA_SIZE + sizeof(BOOK_CACHE_COMMIT_MARKER);

template <typename F, typename T>
bool readPodExact(F& file, T& value) {
  return static_cast<size_t>(file.read(&value, sizeof(value))) == sizeof(value);
}

template <typename F>
bool consumeBoundedString(F& file, const size_t endPosition, std::string* value) {
  uint32_t length = 0;
  if (!readPodExact(file, length)) return false;

  const size_t position = file.position();
  if (position > endPosition || length > endPosition - position) return false;

  if (!value) return file.seek(position + length);

  value->resize(length);
  return length == 0 || static_cast<size_t>(file.read(value->data(), length)) == length;
}

template <typename F>
bool inspectSpineEntry(F& file, const size_t endPosition, const uint16_t tocCount, uint32_t* cumulativeSize = nullptr,
                       int16_t* parsedTocIndex = nullptr) {
  if (!consumeBoundedString(file, endPosition, nullptr)) return false;
  uint8_t linear = 0;
  uint32_t cumulative = 0;
  int16_t tocIndex = -1;
  if (!readPodExact(file, linear) || linear > 1 || !readPodExact(file, cumulative) || !readPodExact(file, tocIndex) ||
      file.position() != endPosition || tocIndex < -1 || tocIndex >= static_cast<int32_t>(tocCount)) {
    return false;
  }
  if (cumulativeSize) *cumulativeSize = cumulative;
  if (parsedTocIndex) *parsedTocIndex = tocIndex;
  return true;
}

template <typename F>
bool inspectTocEntry(F& file, const size_t endPosition, const uint16_t spineCount) {
  if (!consumeBoundedString(file, endPosition, nullptr) || !consumeBoundedString(file, endPosition, nullptr) ||
      !consumeBoundedString(file, endPosition, nullptr)) {
    return false;
  }
  uint8_t level = 0;
  int16_t spineIndex = -1;
  return readPodExact(file, level) && readPodExact(file, spineIndex) && file.position() == endPosition && level > 0 &&
         spineIndex >= -1 && spineIndex < static_cast<int32_t>(spineCount);
}

bool readSpineEntryChecked(HalFile& file, const size_t endPosition, const uint16_t tocCount,
                           BookMetadataCache::SpineEntry& entry) {
  BookMetadataCache::SpineEntry parsed;
  uint8_t linear = 0;
  if (!consumeBoundedString(file, endPosition, &parsed.href) || !readPodExact(file, linear) || linear > 1 ||
      !readPodExact(file, parsed.cumulativeSize) || !readPodExact(file, parsed.tocIndex) ||
      file.position() != endPosition || parsed.tocIndex < -1 || parsed.tocIndex >= static_cast<int32_t>(tocCount)) {
    return false;
  }
  parsed.linear = linear != 0;
  entry = std::move(parsed);
  return true;
}

bool readTocEntryChecked(HalFile& file, const size_t endPosition, const uint16_t spineCount,
                         BookMetadataCache::TocEntry& entry) {
  BookMetadataCache::TocEntry parsed;
  if (!consumeBoundedString(file, endPosition, &parsed.title) ||
      !consumeBoundedString(file, endPosition, &parsed.href) ||
      !consumeBoundedString(file, endPosition, &parsed.anchor) || !readPodExact(file, parsed.level) ||
      !readPodExact(file, parsed.spineIndex) || file.position() != endPosition || parsed.level == 0 ||
      parsed.spineIndex < -1 || parsed.spineIndex >= static_cast<int32_t>(spineCount)) {
    return false;
  }
  entry = std::move(parsed);
  return true;
}

bool consumeScratchString(HalFile& file, const size_t fileSize) {
  uint32_t length = 0;
  if (!readPodExact(file, length) || length > BOOK_CACHE_MAX_ENTRY_SIZE) return false;

  const size_t position = file.position();
  return position <= fileSize && length <= fileSize - position && file.seek(position + length);
}

bool validateSpineScratchFile(HalFile& file, const uint16_t expectedCount) {
  const size_t fileSize = file.size();
  if (!file.seek(0)) return false;

  for (uint16_t i = 0; i < expectedCount; ++i) {
    uint8_t linear = 0;
    uint32_t cumulativeSize = 0;
    int16_t tocIndex = -1;
    if (!consumeScratchString(file, fileSize) || !readPodExact(file, linear) || linear > 1 ||
        !readPodExact(file, cumulativeSize) || !readPodExact(file, tocIndex) || cumulativeSize != 0 || tocIndex != -1) {
      return false;
    }
  }
  return file.position() == fileSize && file.seek(0);
}

bool validEntryBounds(const uint32_t lutOffset, const uint32_t entryCount, const size_t dataEndOffset,
                      const uint32_t start, const uint32_t end) {
  const uint64_t dataStart = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(entryCount) * sizeof(uint32_t);
  return start >= dataStart && end > start && end <= dataEndOffset && end - start <= BOOK_CACHE_MAX_ENTRY_SIZE;
}

bool readEntryBounds(HalFile& file, const uint32_t lutOffset, const uint32_t entryCount, const uint32_t index,
                     const size_t dataEndOffset, size_t& entryStart, size_t& entryEnd) {
  if (index >= entryCount) return false;
  const uint64_t lutPosition = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(index) * sizeof(uint32_t);
  if (lutPosition > SIZE_MAX || !file.seek(static_cast<size_t>(lutPosition))) return false;

  uint32_t start = 0;
  if (!readPodExact(file, start)) return false;

  uint32_t end = 0;
  if (index + 1 < entryCount) {
    if (!readPodExact(file, end)) return false;
  } else {
    if (dataEndOffset > UINT32_MAX) return false;
    end = static_cast<uint32_t>(dataEndOffset);
  }

  if (!validEntryBounds(lutOffset, entryCount, dataEndOffset, start, end)) return false;
  entryStart = start;
  entryEnd = end;
  return true;
}

bool validateBookCacheCandidate(const char* path, void* context) {
  const auto* expectedSourceIdentity = static_cast<const ZipFile::SourceIdentity*>(context);
  if (!expectedSourceIdentity) return false;

  HalFile file;
  if (!Storage.openFileForRead("BMC", path, file)) return false;

  bool valid = false;
  do {
    const size_t fileSize = file.size();
    if (fileSize < BOOK_CACHE_MIN_FILE_SIZE) break;

    uint8_t version = 0;
    uint32_t candidateLutOffset = 0;
    uint16_t candidateSpineCount = 0;
    uint16_t candidateTocCount = 0;
    SourceIdentityCodec::Payload identityPayload{};
    uint32_t identityChecksum = 0;
    ZipFile::SourceIdentity storedIdentity;
    if (!readPodExact(file, version) || version != BOOK_CACHE_VERSION || !readPodExact(file, candidateLutOffset) ||
        !readPodExact(file, candidateSpineCount) || !readPodExact(file, candidateTocCount) ||
        file.read(identityPayload.data(), identityPayload.size()) != static_cast<int>(identityPayload.size()) ||
        !readPodExact(file, identityChecksum) ||
        identityChecksum != SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()) ||
        !SourceIdentityCodec::decodePayload(identityPayload.data(), identityPayload.size(), storedIdentity) ||
        storedIdentity != *expectedSourceIdentity) {
      break;
    }

    const size_t minimumLutOffset = BOOK_CACHE_FIXED_HEADER_SIZE + BOOK_CACHE_MIN_METADATA_SIZE;
    if (candidateLutOffset < minimumLutOffset ||
        candidateLutOffset - BOOK_CACHE_FIXED_HEADER_SIZE > BOOK_CACHE_MAX_METADATA_SIZE) {
      break;
    }

    const uint32_t entryCount = static_cast<uint32_t>(candidateSpineCount) + candidateTocCount;
    const uint64_t lutSize = static_cast<uint64_t>(entryCount) * sizeof(uint32_t);
    const uint64_t dataEnd = fileSize - sizeof(BOOK_CACHE_COMMIT_MARKER);
    const uint64_t dataStart = static_cast<uint64_t>(candidateLutOffset) + lutSize;
    if (dataStart > dataEnd || dataEnd > UINT32_MAX) break;

    uint32_t commitMarker = 0;
    if (!file.seek(fileSize - sizeof(commitMarker)) || !readPodExact(file, commitMarker) ||
        commitMarker != BOOK_CACHE_COMMIT_MARKER || !file.seek(BOOK_CACHE_FIXED_HEADER_SIZE) ||
        !consumeBoundedString(file, candidateLutOffset, nullptr) ||
        !consumeBoundedString(file, candidateLutOffset, nullptr) ||
        !consumeBoundedString(file, candidateLutOffset, nullptr) ||
        !consumeBoundedString(file, candidateLutOffset, nullptr) ||
        !consumeBoundedString(file, candidateLutOffset, nullptr) || file.position() != candidateLutOffset) {
      break;
    }

    if (entryCount == 0) {
      valid = dataStart == dataEnd;
      break;
    }

    const size_t dataEndOffset = static_cast<size_t>(dataEnd);
    serialization::BufferedFileReader input(file, BUILD_IO_BUFFER_SIZE);
    std::array<uint32_t, BOOK_CACHE_LUT_CHUNK_SIZE + 1> offsets;
    uint32_t previousCumulativeSize = 0;
    uint32_t index = 0;
    valid = true;
    while (valid && index < entryCount) {
      const size_t chunkCount = std::min<size_t>(BOOK_CACHE_LUT_CHUNK_SIZE, entryCount - index);
      const bool hasNextOffset = index + chunkCount < entryCount;
      const size_t offsetCount = chunkCount + (hasNextOffset ? 1 : 0);
      const uint64_t lutPosition =
          static_cast<uint64_t>(candidateLutOffset) + static_cast<uint64_t>(index) * sizeof(uint32_t);
      if (lutPosition > SIZE_MAX || !input.seek(static_cast<size_t>(lutPosition)) ||
          input.read(offsets.data(), offsetCount * sizeof(uint32_t)) != offsetCount * sizeof(uint32_t)) {
        valid = false;
        break;
      }

      for (size_t withinChunk = 0; withinChunk < chunkCount; ++withinChunk, ++index) {
        const uint32_t entryStart = offsets[withinChunk];
        const uint32_t entryEnd =
            withinChunk + 1 < offsetCount ? offsets[withinChunk + 1] : static_cast<uint32_t>(dataEndOffset);
        if (!validEntryBounds(candidateLutOffset, entryCount, dataEndOffset, entryStart, entryEnd) ||
            (index == 0 && entryStart != dataStart) || (input.position() != entryStart && !input.seek(entryStart))) {
          valid = false;
          break;
        }

        if (index < candidateSpineCount) {
          uint32_t cumulativeSize = 0;
          if (!inspectSpineEntry(input, entryEnd, candidateTocCount, &cumulativeSize) ||
              cumulativeSize < previousCumulativeSize) {
            valid = false;
            break;
          }
          previousCumulativeSize = cumulativeSize;
        } else if (!inspectTocEntry(input, entryEnd, candidateSpineCount)) {
          valid = false;
          break;
        }
      }
    }
  } while (false);

  const bool closed = file.close();
  return valid && closed;
}

void updateBookCacheDigest(void* context, const uint8_t* data, const size_t size) {
  auto* digest = static_cast<StagedFileTransaction::Digest*>(context);
  StagedFileTransaction::updateDigest(*digest, data, size);
}

// Entry (de)serializers, templated so they run over HalFile and the Buffered*
// wrappers alike (two instantiations each -- a few hundred bytes of flash, in
// exchange for the build path streaming at SD speed instead of per-pod).
template <typename F>
uint32_t writeSpineEntryTo(F& file, const BookMetadataCache::SpineEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, static_cast<uint8_t>(entry.linear ? 1 : 0));
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

template <typename F>
uint32_t writeTocEntryTo(F& file, const BookMetadataCache::TocEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

template <typename F>
BookMetadataCache::SpineEntry readSpineEntryFrom(F& file) {
  BookMetadataCache::SpineEntry entry;
  uint8_t linear = 0;
  serialization::readString(file, entry.href);
  serialization::readPod(file, linear);
  entry.linear = linear != 0;
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

template <typename F>
BookMetadataCache::TocEntry readTocEntryFrom(F& file) {
  BookMetadataCache::TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}

template <typename F, typename T>
bool readScratchPod(F& file, T& value) {
  return file.read(&value, sizeof(value)) == sizeof(value);
}

template <typename F>
bool consumeScratchStringFrom(F& file, const size_t fileSize) {
  uint32_t length = 0;
  if (!readScratchPod(file, length) || length > BOOK_CACHE_MAX_ENTRY_SIZE) return false;
  const size_t position = file.position();
  return position <= fileSize && length <= fileSize - position && file.seek(position + length);
}

template <typename F>
bool validateSpineScratchEntry(F& file, const size_t fileSize) {
  uint8_t linear = 0;
  uint32_t cumulativeSize = 0;
  int16_t tocIndex = -1;
  return consumeScratchStringFrom(file, fileSize) && readScratchPod(file, linear) && linear <= 1 &&
         readScratchPod(file, cumulativeSize) && readScratchPod(file, tocIndex) && cumulativeSize == 0 &&
         tocIndex == -1;
}

template <typename F>
bool validateTocScratchEntry(F& file, const size_t fileSize, const uint16_t spineCount) {
  uint8_t level = 0;
  int16_t spineIndex = -1;
  return consumeScratchStringFrom(file, fileSize) && consumeScratchStringFrom(file, fileSize) &&
         consumeScratchStringFrom(file, fileSize) && readScratchPod(file, level) && readScratchPod(file, spineIndex) &&
         level > 0 && spineIndex >= -1 && spineIndex < static_cast<int32_t>(spineCount);
}
}  // namespace

class BookMetadataCache::LoadState {
 public:
  size_t fileSize = 0;
  size_t dataStart = 0;
  size_t dataEnd = 0;
  uint32_t entryCount = 0;
  uint32_t nextEntry = 0;
  uint32_t previousCumulativeSize = 0;
  std::array<uint32_t, BOOK_CACHE_LUT_CHUNK_SIZE + 1> offsets;
  size_t chunkCount = 0;
  size_t chunkOffsetCount = 0;
  size_t chunkCursor = 0;
};

class BookMetadataCache::BuildState {
 public:
  enum class Phase : uint8_t {
    ValidateSpine,
    ValidateToc,
    OpenOutput,
    SpineLut,
    TocLut,
    TocMap,
    OpenZip,
    SizeTargets,
    BatchSizes,
    WriteSpines,
    WriteToc,
    Finalize,
  };

  const std::string* epubPath = nullptr;
  const BookMetadata* metadata = nullptr;
  ZipFile::SourceIdentity sourceIdentity{};
  SourceIdentityCodec::Payload identityPayload{};
  uint32_t identityChecksum = 0;
  uint32_t metadataSize = 0;
  uint32_t lutOffset = 0;
  uint32_t lutSize = 0;
  uint32_t spineBytes = 0;
  size_t scratchSize = 0;
  uint32_t nextEntry = 0;
  uint32_t cumulativeSize = 0;
  int lastSpineTocIndex = -1;
  int missingTocCount = 0;
  bool outputStarted = false;
  bool useBatchSizes = false;
  Phase phase = Phase::ValidateSpine;
  std::unique_ptr<serialization::BufferedFileWriter> bookOut;
  std::unique_ptr<serialization::BufferedFileReader> spineIn;
  std::unique_ptr<serialization::BufferedFileReader> tocIn;
  std::unique_ptr<int16_t[]> spineToTocIndex;
  std::unique_ptr<uint32_t[]> spineSizes;
  std::unique_ptr<ZipFile::SizeTarget[]> sizeTargets;
  std::unique_ptr<ZipFile> zip;
  StagedFileTransaction::Digest outputDigest;
};

BookMetadataCache::BookMetadataCache(std::string cachePath)
    : cachePath(std::move(cachePath)),
      lutOffset(0),
      dataEndOffset(0),
      loadedFileSize(0),
      spineCount(0),
      tocCount(0),
      loaded(false),
      buildMode(false),
      lastLoadStatus(LoadStatus::Missing) {}

BookMetadataCache::~BookMetadataCache() {
  cancelBuildBookBin();
  cancelLoad();
  cancelWrite();
}

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  // Wrapper OOM is fine: createSpineEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(spineFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  // Explicit close() required: member variable persists beyond function scope
  const bool closed = spineFile.close();
  if (!flushed || !closed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
  }
  return flushed && closed;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!validateSpineScratchFile(spineFile, spineCount)) {
    LOG_ERR("BMC", "Spine tmp file is truncated or corrupt");
    spineFile.close();
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      const uint32_t scratchOffset = spineFile.position();
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      idx.scratchOffset = scratchOffset;
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  // Wrapper OOM is fine: createTocEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(tocFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endTocPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  const bool closed = tocFile.close();
  if (!flushed || !closed) {
    LOG_ERR("BMC", "Failed writing toc tmp file");
  }
  // Explicit close() required: member variables persist beyond function scope
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return flushed && closed;
}

bool BookMetadataCache::restartTocPass() {
  if (!endTocPass()) return false;
  tocCount = 0;
  return beginTocPass();
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

void BookMetadataCache::cancelWrite() {
  cancelBuildBookBin();
  passOut.reset();
  if (spineFile) spineFile.close();
  if (tocFile) tocFile.close();
  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;
  buildMode = false;
  cleanupTmpFiles();
}

bool BookMetadataCache::beginBuildBookBin(const std::string& epubPath, const BookMetadata& metadata,
                                          const ZipFile::SourceIdentity& sourceIdentity) {
  cancelBuildBookBin();
  const uint64_t metadataSize64 = BOOK_CACHE_MIN_METADATA_SIZE + static_cast<uint64_t>(metadata.title.size()) +
                                  metadata.author.size() + metadata.language.size() + metadata.coverItemHref.size() +
                                  metadata.textReferenceHref.size();
  if (metadataSize64 > BOOK_CACHE_MAX_METADATA_SIZE) {
    LOG_ERR("BMC", "Book metadata is too large to cache safely (%llu bytes)",
            static_cast<unsigned long long>(metadataSize64));
    return false;
  }

  auto next = makeUniqueNoThrow<BuildState>();
  if (!next || !SourceIdentityCodec::encodePayload(sourceIdentity, next->identityPayload)) return false;
  next->epubPath = &epubPath;
  next->metadata = &metadata;
  next->sourceIdentity = sourceIdentity;
  next->identityChecksum = SourceIdentityCodec::crc32(next->identityPayload.data(), next->identityPayload.size());
  next->metadataSize = static_cast<uint32_t>(metadataSize64);

  // Refuse to publish metadata parsed from an EPUB that changed during the
  // indexing passes. This check happens before book.bin is truncated.
  ZipFile identityZip(epubPath);
  ZipFile::SourceIdentity currentIdentity;
  if (!identityZip.getSourceIdentity(currentIdentity) || currentIdentity != sourceIdentity) {
    LOG_ERR("BMC", "EPUB changed while metadata was being indexed");
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) return false;
  next->scratchSize = spineFile.size();
  next->spineIn = makeUniqueNoThrow<serialization::BufferedFileReader>(spineFile, BUILD_IO_BUFFER_SIZE);
  if (!next->spineIn) {
    spineFile.close();
    return false;
  }
  buildState = std::move(next);
  return true;
}

BookMetadataCache::BuildStepResult BookMetadataCache::stepBuildBookBin(const size_t maxEntries) {
  if (!buildState || maxEntries == 0) return BuildStepResult::Error;

  const auto fail = [this](const char* message) {
    (void)message;
    LOG_ERR("BMC", "%s", message);
    cancelBuildBookBin();
    return BuildStepResult::Error;
  };
  BuildState& state = *buildState;

  if (state.phase == BuildState::Phase::ValidateSpine) {
    size_t processed = 0;
    while (state.nextEntry < spineCount && processed++ < maxEntries) {
      if (!validateSpineScratchEntry(*state.spineIn, state.scratchSize)) {
        return fail("Spine tmp file is truncated or corrupt");
      }
      ++state.nextEntry;
    }
    if (state.nextEntry < spineCount) return BuildStepResult::InProgress;
    if (state.spineIn->position() != state.scratchSize) return fail("Spine tmp file has trailing data");
    state.spineIn.reset();
    if (!spineFile.close() || !Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
      return fail("Could not open TOC tmp file");
    }
    state.scratchSize = tocFile.size();
    state.tocIn = makeUniqueNoThrow<serialization::BufferedFileReader>(tocFile, BUILD_IO_BUFFER_SIZE);
    if (!state.tocIn) return fail("Not enough memory to validate TOC tmp file");
    state.nextEntry = 0;
    state.phase = BuildState::Phase::ValidateToc;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::ValidateToc) {
    size_t processed = 0;
    while (state.nextEntry < tocCount && processed++ < maxEntries) {
      if (!validateTocScratchEntry(*state.tocIn, state.scratchSize, spineCount)) {
        return fail("TOC tmp file is truncated or corrupt");
      }
      ++state.nextEntry;
    }
    if (state.nextEntry < tocCount) return BuildStepResult::InProgress;
    if (state.tocIn->position() != state.scratchSize) return fail("TOC tmp file has trailing data");
    state.tocIn.reset();
    if (!tocFile.close()) return fail("Could not close TOC tmp file");
    state.phase = BuildState::Phase::OpenOutput;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::OpenOutput) {
    // The scratch files are fully validated before the derived final is
    // written. A cancellation after this point removes the incomplete staging file.
    const std::string stagingPath = cachePath + bookBinStagingFile;
    if (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())) {
      return fail("Could not remove stale book.bin staging file");
    }
    if (!Storage.openFileForWrite("BMC", stagingPath, bookFile)) {
      return fail("Could not create book.bin");
    }
    state.outputStarted = true;
    if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile) ||
        !Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
      return fail("Could not reopen metadata scratch files");
    }
    state.bookOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(bookFile, BUILD_IO_BUFFER_SIZE,
                                                                         updateBookCacheDigest, &state.outputDigest);
    state.spineIn = makeUniqueNoThrow<serialization::BufferedFileReader>(spineFile, BUILD_IO_BUFFER_SIZE);
    state.tocIn = makeUniqueNoThrow<serialization::BufferedFileReader>(tocFile, BUILD_IO_BUFFER_SIZE);
    if (!state.bookOut || !state.spineIn || !state.tocIn) {
      return fail("Not enough memory for buffered book cache build");
    }

    const uint32_t headerASize = BOOK_CACHE_FIXED_HEADER_SIZE;
    state.lutSize = sizeof(uint32_t) * static_cast<uint32_t>(spineCount + tocCount);
    state.lutOffset = headerASize + state.metadataSize;
    serialization::writePod(*state.bookOut, BOOK_CACHE_VERSION);
    serialization::writePod(*state.bookOut, state.lutOffset);
    serialization::writePod(*state.bookOut, spineCount);
    serialization::writePod(*state.bookOut, tocCount);
    state.bookOut->write(state.identityPayload.data(), state.identityPayload.size());
    serialization::writePod(*state.bookOut, state.identityChecksum);
    serialization::writeString(*state.bookOut, state.metadata->title);
    serialization::writeString(*state.bookOut, state.metadata->author);
    serialization::writeString(*state.bookOut, state.metadata->language);
    serialization::writeString(*state.bookOut, state.metadata->coverItemHref);
    serialization::writeString(*state.bookOut, state.metadata->textReferenceHref);
    state.nextEntry = 0;
    state.phase = BuildState::Phase::SpineLut;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::SpineLut) {
    size_t processed = 0;
    while (state.nextEntry < spineCount && processed++ < maxEntries) {
      const uint32_t position = state.spineIn->position();
      readSpineEntryFrom(*state.spineIn);
      serialization::writePod(*state.bookOut, position + state.lutOffset + state.lutSize);
      ++state.nextEntry;
    }
    if (state.nextEntry < spineCount) return BuildStepResult::InProgress;
    state.spineBytes = static_cast<uint32_t>(state.spineIn->position());
    if (!state.tocIn->seek(0)) return fail("Could not rewind TOC tmp file");
    state.nextEntry = 0;
    state.phase = BuildState::Phase::TocLut;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::TocLut) {
    size_t processed = 0;
    while (state.nextEntry < tocCount && processed++ < maxEntries) {
      const uint32_t position = state.tocIn->position();
      readTocEntryFrom(*state.tocIn);
      serialization::writePod(*state.bookOut, position + state.lutOffset + state.lutSize + state.spineBytes);
      ++state.nextEntry;
    }
    if (state.nextEntry < tocCount) return BuildStepResult::InProgress;

    if (spineCount > 0) {
      state.spineToTocIndex.reset(new (std::nothrow) int16_t[spineCount]);
      if (!state.spineToTocIndex) return fail("Not enough memory for spine to TOC mapping");
      std::fill_n(state.spineToTocIndex.get(), spineCount, static_cast<int16_t>(-1));
    }
    if (!state.tocIn->seek(0)) return fail("Could not rewind TOC mapping input");
    state.nextEntry = 0;
    state.phase = BuildState::Phase::TocMap;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::TocMap) {
    size_t processed = 0;
    while (state.nextEntry < tocCount && processed++ < maxEntries) {
      const auto entry = readTocEntryFrom(*state.tocIn);
      if (entry.spineIndex >= 0 && entry.spineIndex < spineCount && state.spineToTocIndex[entry.spineIndex] == -1) {
        state.spineToTocIndex[entry.spineIndex] = static_cast<int16_t>(state.nextEntry);
      }
      ++state.nextEntry;
    }
    if (state.nextEntry < tocCount) return BuildStepResult::InProgress;
    state.phase = BuildState::Phase::OpenZip;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::OpenZip) {
    state.zip = makeUniqueNoThrow<ZipFile>(*state.epubPath);
    if (!state.zip || !state.zip->open() || !state.spineIn->seek(0)) {
      return fail("Could not open EPUB for spine size calculations");
    }
    state.useBatchSizes = spineCount >= LARGE_SPINE_THRESHOLD;
    if (state.useBatchSizes) {
      state.spineSizes.reset(new (std::nothrow) uint32_t[spineCount]);
      state.sizeTargets.reset(new (std::nothrow) ZipFile::SizeTarget[spineCount]);
      if (!state.spineSizes || !state.sizeTargets) return fail("Not enough memory for batch spine sizes");
      std::fill_n(state.spineSizes.get(), spineCount, 0U);
      state.nextEntry = 0;
      state.phase = BuildState::Phase::SizeTargets;
    } else {
      state.nextEntry = 0;
      state.phase = BuildState::Phase::WriteSpines;
    }
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::SizeTargets) {
    size_t processed = 0;
    while (state.nextEntry < spineCount && processed++ < maxEntries) {
      const auto entry = readSpineEntryFrom(*state.spineIn);
      const std::string path = FsHelpers::normalisePath(entry.href);
      if (path.size() > UINT16_MAX) return fail("Spine path exceeds ZIP lookup bounds");
      state.sizeTargets[state.nextEntry] = {ZipFile::fnvHash64(path.c_str(), path.size()),
                                            static_cast<uint16_t>(path.size()), static_cast<uint16_t>(state.nextEntry)};
      ++state.nextEntry;
    }
    if (state.nextEntry < spineCount) return BuildStepResult::InProgress;
    std::sort(state.sizeTargets.get(), state.sizeTargets.get() + spineCount,
              [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
                return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
              });
    state.phase = BuildState::Phase::BatchSizes;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::BatchSizes) {
    const int matched =
        state.zip->fillUncompressedSizes(state.sizeTargets.get(), spineCount, state.spineSizes.get(), spineCount);
    (void)matched;
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);
    if (!state.spineIn->seek(0)) return fail("Could not rewind spine tmp file");
    state.nextEntry = 0;
    state.phase = BuildState::Phase::WriteSpines;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::WriteSpines) {
    size_t processed = 0;
    while (state.nextEntry < spineCount && processed++ < maxEntries) {
      auto entry = readSpineEntryFrom(*state.spineIn);
      entry.tocIndex = state.spineToTocIndex ? state.spineToTocIndex[state.nextEntry] : -1;
      if (entry.tocIndex == -1) {
        ++state.missingTocCount;
        entry.tocIndex = static_cast<int16_t>(state.lastSpineTocIndex);
      }
      state.lastSpineTocIndex = entry.tocIndex;

      size_t itemSize = state.useBatchSizes ? state.spineSizes[state.nextEntry] : 0;
      if (!state.useBatchSizes || itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(entry.href);
        if (!state.zip->getInflatedFileSize(path.c_str(), &itemSize)) {
          return fail("Could not get size for spine item");
        }
      }
      if (itemSize > std::numeric_limits<uint32_t>::max() - static_cast<uint64_t>(state.cumulativeSize)) {
        return fail("Cumulative spine size exceeds cache format limit");
      }
      state.cumulativeSize += static_cast<uint32_t>(itemSize);
      entry.cumulativeSize = state.cumulativeSize;
      writeSpineEntryTo(*state.bookOut, entry);
      ++state.nextEntry;
    }
    if (state.nextEntry < spineCount) return BuildStepResult::InProgress;
    if (state.missingTocCount > 0) {
      LOG_DBG("BMC", "%d/%d spine items have no direct TOC entry; reused the preceding section title",
              state.missingTocCount, spineCount);
    }
    state.zip->close();
    state.zip.reset();
    if (!state.tocIn->seek(0)) return fail("Could not rewind TOC output input");
    state.nextEntry = 0;
    state.phase = BuildState::Phase::WriteToc;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::WriteToc) {
    size_t processed = 0;
    while (state.nextEntry < tocCount && processed++ < maxEntries) {
      writeTocEntryTo(*state.bookOut, readTocEntryFrom(*state.tocIn));
      ++state.nextEntry;
    }
    if (state.nextEntry < tocCount) return BuildStepResult::InProgress;
    state.phase = BuildState::Phase::Finalize;
    return BuildStepResult::InProgress;
  }

  if (state.phase == BuildState::Phase::Finalize) {
    serialization::writePod(*state.bookOut, BOOK_CACHE_COMMIT_MARKER);
    const bool written = state.bookOut->flush();
    state.bookOut.reset();
    state.spineIn.reset();
    state.tocIn.reset();
    const bool closed = bookFile.close();
    const bool spineClosed = spineFile.close();
    const bool tocClosed = tocFile.close();
    if (!written || !closed || !spineClosed || !tocClosed) {
      return fail("Failed writing book.bin");
    }
    const std::string finalPath = cachePath + bookBinFile;
    const std::string stagingPath = cachePath + bookBinStagingFile;
    const std::string backupPath = cachePath + bookBinBackupFile;
    const auto published =
        StagedFileTransaction::publishAndVerify(finalPath.c_str(), stagingPath.c_str(), backupPath.c_str(),
                                                state.outputDigest, validateBookCacheCandidate, &state.sourceIdentity);
    if (published != StagedFileTransaction::Status::Published) {
      return fail("Failed publishing book.bin");
    }
    state.outputStarted = false;
    buildState.reset();
    LOG_DBG("BMC", "Successfully built book.bin");
    return BuildStepResult::Built;
  }

  return fail("Invalid book cache build state");
}

void BookMetadataCache::cancelBuildBookBin() {
  if (!buildState) return;
  const bool removeOutput = buildState->outputStarted;
  buildState->bookOut.reset();
  buildState->spineIn.reset();
  buildState->tocIn.reset();
  if (buildState->zip) buildState->zip->close();
  buildState->zip.reset();
  if (bookFile) bookFile.close();
  if (spineFile) spineFile.close();
  if (tocFile) tocFile.close();
  buildState.reset();
  if (removeOutput) Storage.remove((cachePath + bookBinStagingFile).c_str());
}

bool BookMetadataCache::cleanupTmpFiles() const {
  bool cleaned = true;
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    cleaned = Storage.remove(spineBinFile.c_str()) && cleaned;
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    cleaned = Storage.remove(tocBinFile.c_str()) && cleaned;
  }
  const auto bookStagingFile = cachePath + bookBinStagingFile;
  if (Storage.exists(bookStagingFile.c_str())) {
    cleaned = Storage.remove(bookStagingFile.c_str()) && cleaned;
  }
  return cleaned;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  return writeSpineEntryTo(file, entry);
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  return writeTocEntryTo(file, entry);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href, const bool linear) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, linear, 0, -1);
  if (passOut) {
    writeSpineEntryTo(*passOut, entry);
  } else {
    writeSpineEntry(spineFile, entry);
  }
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      if (spineFile.seek(it->scratchOffset) && readSpineEntry(spineFile).href == href) {
        spineIndex = it->spineIndex;
        break;
      }
      ++it;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level == 0 ? 1 : level, spineIndex);
  if (passOut) {
    writeTocEntryTo(*passOut, entry);
  } else {
    writeTocEntry(tocFile, entry);
  }
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

BookMetadataCache::LoadStatus BookMetadataCache::failLoad(const LoadStatus status) {
  loadState.reset();
  bookFile.close();
  loaded = false;
  spineCumulativeCache.reset();
  spineTocCache.reset();
  dataEndOffset = 0;
  loadedFileSize = 0;
  lastLoadStatus = status;
  return status;
}

BookMetadataCache::LoadStepResult BookMetadataCache::finishLoad() {
  if (!loadState) return LoadStepResult::Error;

  const size_t fileSize = loadState->fileSize;
  const size_t dataEnd = loadState->dataEnd;
  uint32_t commitMarker = 0;
  if (bookFile.size() != fileSize || !bookFile.seek(fileSize - sizeof(commitMarker)) ||
      !readPodExact(bookFile, commitMarker) || commitMarker != BOOK_CACHE_COMMIT_MARKER ||
      !bookFile.seek(BOOK_CACHE_FIXED_HEADER_SIZE) || !consumeBoundedString(bookFile, lutOffset, &coreMetadata.title) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.author) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.language) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.coverItemHref) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.textReferenceHref) || bookFile.position() != lutOffset) {
    coreMetadata = {};
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  dataEndOffset = dataEnd;
  loadedFileSize = fileSize;
  loadState.reset();
  loaded = true;
  lastLoadStatus = LoadStatus::Loaded;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return LoadStepResult::Loaded;
}

BookMetadataCache::LoadStepResult BookMetadataCache::beginLoad(const ZipFile::SourceIdentity& expectedSourceIdentity) {
  loadState.reset();
  if (bookFile) bookFile.close();
  loaded = false;
  coreMetadata = {};
  spineCumulativeCache.reset();
  spineTocCache.reset();
  spineCount = 0;
  tocCount = 0;
  lutOffset = 0;
  dataEndOffset = 0;
  loadedFileSize = 0;
  lastLoadStatus = LoadStatus::Missing;
  const std::string path = cachePath + bookBinFile;
  const std::string backupPath = cachePath + bookBinBackupFile;
  auto recoveryIdentity = expectedSourceIdentity;
  const auto recovered =
      StagedFileTransaction::recover(path.c_str(), backupPath.c_str(), validateBookCacheCandidate, &recoveryIdentity);
  if (recovered == StagedFileTransaction::Status::IoError) {
    LOG_ERR("BMC", "Could not recover book.bin cache");
  }
  if (!Storage.openFileForRead("BMC", path, bookFile)) {
    if (Storage.exists(path.c_str())) lastLoadStatus = LoadStatus::IoError;
    return LoadStepResult::Error;
  }

  const size_t fileSize = bookFile.size();
  if (fileSize < sizeof(BOOK_CACHE_VERSION)) {
    LOG_DBG("BMC", "Cache file is truncated");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  uint8_t version = 0;
  if (!readPodExact(bookFile, version)) {
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }
  if (version < BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Legacy cache version: %d", version);
    failLoad(LoadStatus::LegacyVersion);
    return LoadStepResult::Error;
  }
  if (version > BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Newer cache version: %d", version);
    failLoad(LoadStatus::NewerVersion);
    return LoadStepResult::Error;
  }

  if (fileSize < BOOK_CACHE_MIN_FILE_SIZE) {
    LOG_DBG("BMC", "Cache file is truncated");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  uint32_t commitMarker = 0;
  if (!bookFile.seek(fileSize - sizeof(commitMarker)) || !readPodExact(bookFile, commitMarker) ||
      commitMarker != BOOK_CACHE_COMMIT_MARKER || !bookFile.seek(sizeof(version))) {
    LOG_DBG("BMC", "Cache commit marker is missing");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  if (!readPodExact(bookFile, lutOffset) || !readPodExact(bookFile, spineCount) || !readPodExact(bookFile, tocCount)) {
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  SourceIdentityCodec::Payload identityPayload{};
  uint32_t identityChecksum = 0;
  if (bookFile.read(identityPayload.data(), identityPayload.size()) != static_cast<int>(identityPayload.size()) ||
      !readPodExact(bookFile, identityChecksum)) {
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }
  ZipFile::SourceIdentity storedIdentity;
  if (identityChecksum != SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()) ||
      !SourceIdentityCodec::decodePayload(identityPayload.data(), identityPayload.size(), storedIdentity)) {
    LOG_ERR("BMC", "Cache source identity is corrupt");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  if (storedIdentity != expectedSourceIdentity) {
    LOG_ERR("BMC", "Backing EPUB no longer matches book cache");
    failLoad(LoadStatus::SourceMismatch);
    return LoadStepResult::Error;
  }

  const size_t minimumLutOffset = BOOK_CACHE_FIXED_HEADER_SIZE + BOOK_CACHE_MIN_METADATA_SIZE;
  if (lutOffset < minimumLutOffset || lutOffset - BOOK_CACHE_FIXED_HEADER_SIZE > BOOK_CACHE_MAX_METADATA_SIZE) {
    LOG_DBG("BMC", "Cache metadata bounds are invalid");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  const uint64_t lutSize = static_cast<uint64_t>(spineCount + tocCount) * sizeof(uint32_t);
  const uint64_t dataEnd = fileSize - sizeof(BOOK_CACHE_COMMIT_MARKER);
  const uint64_t dataStart = static_cast<uint64_t>(lutOffset) + lutSize;
  if (dataStart > dataEnd || dataEnd > UINT32_MAX) {
    LOG_DBG("BMC", "Cache LUT bounds are invalid");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  // Validate without allocating first. A corrupt length must never drive a
  // large std::string::resize on this memory-constrained target.
  if (!bookFile.seek(BOOK_CACHE_FIXED_HEADER_SIZE) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      !consumeBoundedString(bookFile, lutOffset, nullptr) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      !consumeBoundedString(bookFile, lutOffset, nullptr) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      bookFile.position() != lutOffset) {
    LOG_DBG("BMC", "Cache metadata is truncated or malformed");
    failLoad(LoadStatus::Invalid);
    return LoadStepResult::Error;
  }

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  if (spineCount > 0 && spineCount <= MAX_CACHED_SPINE_SUMMARIES) {
    spineCumulativeCache.reset(new (std::nothrow) uint32_t[spineCount]);
    spineTocCache.reset(new (std::nothrow) int16_t[spineCount]);
    if (!spineCumulativeCache || !spineTocCache) {
      spineCumulativeCache.reset();
      spineTocCache.reset();
      LOG_DBG("BMC", "Not enough RAM for spine summary cache; using SD fallback");
    }
  }
  if (entryCount == 0) {
    if (dataStart != dataEnd) {
      LOG_DBG("BMC", "Cache has unreferenced entry data");
      failLoad(LoadStatus::Invalid);
      return LoadStepResult::Error;
    }
  }

  loadState = makeUniqueNoThrow<LoadState>();
  if (!loadState) {
    LOG_ERR("BMC", "Not enough memory for cooperative cache validation");
    failLoad(LoadStatus::IoError);
    return LoadStepResult::Error;
  }
  loadState->fileSize = fileSize;
  loadState->dataStart = static_cast<size_t>(dataStart);
  loadState->dataEnd = static_cast<size_t>(dataEnd);
  loadState->entryCount = entryCount;

  if (entryCount == 0) return finishLoad();
  return LoadStepResult::InProgress;
}

BookMetadataCache::LoadStepResult BookMetadataCache::stepLoad(const size_t maxEntries) {
  if (!loadState) return loaded ? LoadStepResult::Loaded : LoadStepResult::Error;
  if (maxEntries == 0) return LoadStepResult::InProgress;

  size_t processed = 0;
  while (processed < maxEntries && loadState->nextEntry < loadState->entryCount) {
    if (loadState->chunkCursor >= loadState->chunkCount) {
      const uint32_t base = loadState->nextEntry;
      loadState->chunkCount = std::min<size_t>(BOOK_CACHE_LUT_CHUNK_SIZE, loadState->entryCount - loadState->nextEntry);
      const bool hasNextOffset = base + loadState->chunkCount < loadState->entryCount;
      loadState->chunkOffsetCount = loadState->chunkCount + (hasNextOffset ? 1 : 0);
      loadState->chunkCursor = 0;
      const uint64_t lutPosition = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(base) * sizeof(uint32_t);
      if (lutPosition > SIZE_MAX || !bookFile.seek(static_cast<size_t>(lutPosition)) ||
          bookFile.read(loadState->offsets.data(), loadState->chunkOffsetCount * sizeof(uint32_t)) !=
              static_cast<int>(loadState->chunkOffsetCount * sizeof(uint32_t))) {
        LOG_DBG("BMC", "Cache entry LUT is malformed");
        failLoad(LoadStatus::Invalid);
        return LoadStepResult::Error;
      }
    }

    const uint32_t index = loadState->nextEntry;
    const size_t withinChunk = loadState->chunkCursor;
    const uint32_t entryStart = loadState->offsets[withinChunk];
    const uint32_t entryEnd = withinChunk + 1 < loadState->chunkOffsetCount ? loadState->offsets[withinChunk + 1]
                                                                            : static_cast<uint32_t>(loadState->dataEnd);
    if (!validEntryBounds(lutOffset, loadState->entryCount, loadState->dataEnd, entryStart, entryEnd) ||
        (index == 0 && entryStart != loadState->dataStart) ||
        (bookFile.position() != entryStart && !bookFile.seek(entryStart))) {
      LOG_DBG("BMC", "Cache entry LUT is malformed");
      failLoad(LoadStatus::Invalid);
      return LoadStepResult::Error;
    }

    if (index < spineCount) {
      uint32_t cumulativeSize = 0;
      int16_t tocIndex = -1;
      if (!inspectSpineEntry(bookFile, entryEnd, tocCount, &cumulativeSize, &tocIndex) ||
          cumulativeSize < loadState->previousCumulativeSize) {
        LOG_DBG("BMC", "Cache spine entry is malformed");
        failLoad(LoadStatus::Invalid);
        return LoadStepResult::Error;
      }
      if (spineCumulativeCache) {
        spineCumulativeCache[index] = cumulativeSize;
        spineTocCache[index] = tocIndex;
      }
      loadState->previousCumulativeSize = cumulativeSize;
    } else if (!inspectTocEntry(bookFile, entryEnd, spineCount)) {
      LOG_DBG("BMC", "Cache TOC entry is malformed");
      failLoad(LoadStatus::Invalid);
      return LoadStepResult::Error;
    }

    ++loadState->nextEntry;
    ++loadState->chunkCursor;
    ++processed;
  }

  if (loadState->nextEntry < loadState->entryCount) return LoadStepResult::InProgress;
  return finishLoad();
}

void BookMetadataCache::cancelLoad() {
  if (!loadState) return;
  loadState.reset();
  bookFile.close();
  loaded = false;
  coreMetadata = {};
  spineCumulativeCache.reset();
  spineTocCache.reset();
  dataEndOffset = 0;
  loadedFileSize = 0;
}

BookMetadataCache::LoadStatus BookMetadataCache::load(const ZipFile::SourceIdentity& expectedSourceIdentity) {
  LoadStepResult result = beginLoad(expectedSourceIdentity);
  while (result == LoadStepResult::InProgress) result = stepLoad(BOOK_CACHE_LUT_CHUNK_SIZE);
  return lastLoadStatus;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  size_t entryStart = 0;
  size_t entryEnd = 0;
  SpineEntry entry;
  if (bookFile.size() != loadedFileSize ||
      !readEntryBounds(bookFile, lutOffset, entryCount, static_cast<uint32_t>(index), dataEndOffset, entryStart,
                       entryEnd) ||
      !bookFile.seek(entryStart) || !readSpineEntryChecked(bookFile, entryEnd, tocCount, entry)) {
    LOG_ERR("BMC", "Spine cache changed or became corrupt after load");
    loaded = false;
    lastLoadStatus = LoadStatus::Invalid;
    bookFile.close();
    return {};
  }
  return entry;
}

uint32_t BookMetadataCache::getSpineCumulativeSize(const int index) {
  if (!loaded || index < 0 || index >= static_cast<int>(spineCount)) return 0;
  if (spineCumulativeCache) return spineCumulativeCache[index];
  return getSpineEntry(index).cumulativeSize;
}

int16_t BookMetadataCache::getSpineTocIndex(const int index) {
  if (!loaded || index < 0 || index >= static_cast<int>(spineCount)) return -1;
  if (spineTocCache) return spineTocCache[index];
  return getSpineEntry(index).tocIndex;
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  const uint32_t entryIndex = static_cast<uint32_t>(spineCount) + static_cast<uint32_t>(index);
  size_t entryStart = 0;
  size_t entryEnd = 0;
  TocEntry entry;
  if (bookFile.size() != loadedFileSize ||
      !readEntryBounds(bookFile, lutOffset, entryCount, entryIndex, dataEndOffset, entryStart, entryEnd) ||
      !bookFile.seek(entryStart) || !readTocEntryChecked(bookFile, entryEnd, spineCount, entry)) {
    LOG_ERR("BMC", "TOC cache changed or became corrupt after load");
    loaded = false;
    lastLoadStatus = LoadStatus::Invalid;
    bookFile.close();
    return {};
  }
  return entry;
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  return readSpineEntryFrom(file);
}
