/**
 * XtcParser.cpp
 *
 * Strict reader for the CrossVi-supported XTC/XTCH v1.0 subset.
 */

#include "XtcParser.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <RawSourceIdentity.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "XtcPageLayout.h"

namespace xtc {
namespace {

constexpr size_t IDENTITY_CHUNK_SIZE = 2048;

bool rangeWithin(const uint64_t offset, const uint64_t length, const uint64_t fileSize) {
  return offset <= fileSize && length <= fileSize - offset;
}

bool hasTerminator(const uint8_t* bytes, const size_t length) { return std::memchr(bytes, 0, length) != nullptr; }

bool readValidChapterRange(const std::array<uint8_t, XTC_CHAPTER_SIZE>& bytes, const uint16_t pageCount,
                           uint16_t& startPage, uint16_t& endPage) {
  if (!hasTerminator(bytes.data(), 80)) return false;
  std::memcpy(&startPage, bytes.data() + 0x50, sizeof(startPage));
  std::memcpy(&endPage, bytes.data() + 0x52, sizeof(endPage));
  return startPage != 0 && endPage != 0 && startPage <= endPage && endPage <= pageCount;
}

}  // namespace

XtcParser::XtcParser()
    : m_isOpen(false),
      m_fileSize(0),
      m_chapterCount(0),
      m_hasSourceIdentity(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_chaptersLoaded(false),
      m_lastError(XtcError::OK) {
  std::memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::failOpen(const XtcError error) {
  closeFile();
  m_isOpen = false;
  m_fileSize = 0;
  m_chapterCount = 0;
  m_hasSourceIdentity = false;
  m_sourceIdentity = {};
  m_sourceIdentityHandoff = {};
  m_reusedSourceIdentity = false;
  m_chaptersLoaded = false;
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  m_hasChapters = false;
  m_openPhase = OpenPhase::Idle;
  m_openPageIndex = 0;
  m_openChapterIndex = 0;
  m_openFingerprintBytes = 0;
  m_openFingerprint = {};
  std::memset(&m_header, 0, sizeof(m_header));
  m_lastError = error;
  return error;
}

XtcError XtcParser::beginOpen(const char* filepath, const RawSourceIdentityHandoff* const preparedIdentity) {
  close();
  m_lastError = XtcError::OK;
  if (!filepath || filepath[0] == '\0') return failOpen(XtcError::INVALID_ARGUMENT);
  m_filepath = filepath;

  if (!Storage.openFileForRead("XTC", filepath, m_file)) return failOpen(XtcError::FILE_NOT_FOUND);

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  m_openStartedMs = static_cast<uint32_t>(millis());
  m_openStartFreeHeap = ESP.getFreeHeap();
#endif

  XtcError error = readHeader();
  if (error == XtcError::OK) error = readMetadata();
  const uint64_t tableBytes = static_cast<uint64_t>(m_header.pageCount) * sizeof(PageTableEntry);
  if (error == XtcError::OK && !rangeWithin(m_header.pageTableOffset, tableBytes, m_fileSize)) {
    error = XtcError::OFFSET_OUT_OF_RANGE;
  }
  if (error != XtcError::OK) {
    LOG_DBG("XTC", "Rejected %s: %s", filepath, errorToString(error));
    return failOpen(error);
  }

  if (preparedIdentity && preparedIdentity->matchesOpenFile(m_filepath, m_file)) {
    m_sourceIdentity = preparedIdentity->identity;
    m_sourceIdentityHandoff = *preparedIdentity;
    m_hasSourceIdentity = true;
    m_reusedSourceIdentity = true;
  }

  m_openPageIndex = 0;
  m_openChapterIndex = 0;
  m_openFingerprintBytes = 0;
  m_openFingerprint = {};
  m_openPhase = OpenPhase::PageTable;
  return XtcError::OK;
}

XtcParser::OpenStepResult XtcParser::stepOpen(const size_t maxRecords, const size_t maxFingerprintBytes) {
  if (m_isOpen) return OpenStepResult::Opened;
  if (m_openPhase == OpenPhase::Idle || maxRecords == 0 || maxFingerprintBytes == 0) {
    m_lastError = XtcError::INVALID_ARGUMENT;
    return OpenStepResult::Error;
  }

  XtcError error = XtcError::OK;
  if (m_openPhase == OpenPhase::PageTable) {
    error = validatePageTableStep(maxRecords);
    if (error == XtcError::OK && m_openPageIndex < m_header.pageCount) return OpenStepResult::InProgress;
    if (error == XtcError::OK) m_openPhase = OpenPhase::Chapters;
  } else if (m_openPhase == OpenPhase::Chapters) {
    error = validateChaptersStep(maxRecords);
    if (error == XtcError::OK && m_openChapterIndex < m_chapterCount) return OpenStepResult::InProgress;
    if (error == XtcError::OK) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
      m_validationFinishedMs = static_cast<uint32_t>(millis());
#endif
      if (m_hasSourceIdentity) return completeOpen();
      if (!ensureFileOpen() || !m_file.seek64(0)) error = XtcError::READ_ERROR;
    }
    if (error == XtcError::OK) {
      m_openPhase = OpenPhase::Fingerprint;
      return OpenStepResult::InProgress;
    }
  } else if (m_openPhase == OpenPhase::Fingerprint) {
    error = fingerprintSourceStep(maxFingerprintBytes);
    if (error == XtcError::OK && m_openFingerprintBytes < m_fileSize) return OpenStepResult::InProgress;
    if (error == XtcError::OK) return completeOpen();
  }

  if (error != XtcError::OK) {
    LOG_DBG("XTC", "Rejected %s: %s", m_filepath.c_str(), errorToString(error));
    failOpen(error);
    return OpenStepResult::Error;
  }
  return OpenStepResult::InProgress;
}

XtcParser::OpenStepResult XtcParser::completeOpen() {
  closeFile();
  m_isOpen = true;
  m_openPhase = OpenPhase::Idle;
  m_lastError = XtcError::OK;

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const uint32_t openFinishedMs = static_cast<uint32_t>(millis());
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("XTRM",
          "open bytes=%llu pages=%u validation_ms=%u identity_ms=%u total_ms=%u identity_reused=%u heap_delta=%ld "
          "free_heap=%u max_alloc=%u min_free_heap=%u",
          static_cast<unsigned long long>(m_fileSize), m_header.pageCount,
          static_cast<unsigned>(m_validationFinishedMs - m_openStartedMs),
          m_reusedSourceIdentity ? 0U : static_cast<unsigned>(openFinishedMs - m_validationFinishedMs),
          static_cast<unsigned>(openFinishedMs - m_openStartedMs), m_reusedSourceIdentity ? 1U : 0U,
          static_cast<long>(static_cast<int32_t>(m_openStartFreeHeap) - static_cast<int32_t>(freeHeap)),
          static_cast<unsigned>(freeHeap), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
#endif

  LOG_DBG("XTC", "Opened file: %s (%u pages, %ux%u)", m_filepath.c_str(), m_header.pageCount, m_defaultWidth,
          m_defaultHeight);
  return OpenStepResult::Opened;
}

void XtcParser::cancelOpen() {
  close();
  m_lastError = XtcError::OK;
}

XtcError XtcParser::readCoreMetadata(const char* filepath, std::string& title, std::string& author) {
  title.clear();
  author.clear();
  if (!filepath || filepath[0] == '\0') return XtcError::INVALID_ARGUMENT;

  XtcParser parser;
  parser.m_filepath = filepath;
  if (!Storage.openFileForRead("XTC", filepath, parser.m_file)) return XtcError::FILE_NOT_FOUND;

  XtcError error = parser.readHeader();
  if (error == XtcError::OK) error = parser.readMetadata();
  if (error == XtcError::OK && parser.m_file.fileSize64() != parser.m_fileSize) error = XtcError::READ_ERROR;
  const bool closed = parser.m_file.close();
  if (error != XtcError::OK || !closed) return error == XtcError::OK ? XtcError::READ_ERROR : error;

  title = std::move(parser.m_title);
  author = std::move(parser.m_author);
  return XtcError::OK;
}

void XtcParser::close() {
  closeFile();
  m_isOpen = false;
  m_fileSize = 0;
  m_chapterCount = 0;
  m_hasSourceIdentity = false;
  m_sourceIdentity = {};
  m_sourceIdentityHandoff = {};
  m_reusedSourceIdentity = false;
  m_chaptersLoaded = false;
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  m_hasChapters = false;
  m_defaultWidth = DISPLAY_WIDTH;
  m_defaultHeight = DISPLAY_HEIGHT;
  m_bitDepth = 1;
  m_openPhase = OpenPhase::Idle;
  m_openPageIndex = 0;
  m_openChapterIndex = 0;
  m_openFingerprintBytes = 0;
  m_openFingerprint = {};
  std::memset(&m_header, 0, sizeof(m_header));
}

bool XtcParser::ensureFileOpen() {
  if (m_file.isOpen()) return true;
  return Storage.openFileForRead("XTC", m_filepath.c_str(), m_file);
}

void XtcParser::closeFile() {
  if (m_file.isOpen()) m_file.close();
}

XtcError XtcParser::readHeader() {
  m_fileSize = m_file.fileSize64();
  if (m_fileSize < sizeof(XtcHeader) ||
      m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader)) != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }
  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) return XtcError::INVALID_MAGIC;
  if (m_header.versionMajor != 1 || m_header.versionMinor != 0) return XtcError::INVALID_VERSION;
  if (m_header.pageCount == 0 || m_header.readDirection > 2 || m_header.hasMetadata > 1 || m_header.hasThumbnails > 1 ||
      m_header.hasChapters > 1 || m_header.currentPage > m_header.pageCount) {
    return XtcError::CORRUPTED_HEADER;
  }
  if (m_header.pageTableOffset < sizeof(XtcHeader) || m_header.dataOffset < sizeof(XtcHeader) ||
      m_header.pageTableOffset > m_fileSize || m_header.dataOffset > m_fileSize) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  m_bitDepth = m_header.magic == XTCH_MAGIC ? 2 : 1;
  return XtcError::OK;
}

XtcError XtcParser::readMetadata() {
  m_title.clear();
  m_author.clear();
  m_chapterCount = 0;
  m_hasChapters = false;

  if (m_header.hasMetadata == 0) {
    return m_header.hasChapters == 0 ? XtcError::OK : XtcError::INVALID_CHAPTERS;
  }
  if (m_header.metadataOffset < sizeof(XtcHeader) ||
      !rangeWithin(m_header.metadataOffset, XTC_METADATA_SIZE, m_fileSize) || !m_file.seek64(m_header.metadataOffset)) {
    return XtcError::INVALID_METADATA;
  }

  std::array<uint8_t, XTC_METADATA_SIZE> metadata{};
  const int metadataRead = m_file.read(metadata.data(), metadata.size());
  if (metadataRead < 0 || static_cast<size_t>(metadataRead) != metadata.size()) return XtcError::READ_ERROR;
  if (!hasTerminator(metadata.data(), 128) || !hasTerminator(metadata.data() + 128, 64)) {
    return XtcError::INVALID_METADATA;
  }
  m_title.assign(reinterpret_cast<const char*>(metadata.data()),
                 strnlen(reinterpret_cast<const char*>(metadata.data()), 128));
  m_author.assign(reinterpret_cast<const char*>(metadata.data() + 128),
                  strnlen(reinterpret_cast<const char*>(metadata.data() + 128), 64));

  if (m_header.hasChapters == 0) return XtcError::OK;
  std::memcpy(&m_chapterCount, metadata.data() + 196, sizeof(m_chapterCount));
  if (m_chapterCount == 0 || m_chapterCount > m_header.pageCount || m_chapterCount > XTC_MAX_CHAPTERS) {
    return XtcError::INVALID_CHAPTERS;
  }
  uint64_t chapterBytes = static_cast<uint64_t>(m_chapterCount) * XTC_CHAPTER_SIZE;
  if (m_header.chapterOffset < sizeof(XtcHeader) || !rangeWithin(m_header.chapterOffset, chapterBytes, m_fileSize)) {
    return XtcError::INVALID_CHAPTERS;
  }
  m_hasChapters = true;
  return XtcError::OK;
}

XtcError XtcParser::validatePageTableStep(const size_t maxEntries) {
  if (maxEntries == 0 || m_openPageIndex > m_header.pageCount) return XtcError::INVALID_ARGUMENT;
  constexpr size_t PAGE_TABLE_ENTRIES_PER_BLOCK = 16;
  std::array<PageTableEntry, PAGE_TABLE_ENTRIES_PER_BLOCK> entries{};
  size_t validated = 0;
  while (m_openPageIndex < m_header.pageCount && validated < maxEntries) {
    const uint32_t firstPage = m_openPageIndex;
    const size_t entryCount =
        std::min<size_t>({entries.size(), m_header.pageCount - firstPage, maxEntries - validated});
    const size_t bytesToRead = entryCount * sizeof(PageTableEntry);
    const uint64_t blockOffset = m_header.pageTableOffset + static_cast<uint64_t>(firstPage) * sizeof(PageTableEntry);
    if (!m_file.seek64(blockOffset) ||
        m_file.read(reinterpret_cast<uint8_t*>(entries.data()), bytesToRead) != static_cast<int>(bytesToRead)) {
      return XtcError::READ_ERROR;
    }

    for (size_t index = 0; index < entryCount; ++index) {
      PageInfo info;
      // Opening proves every table entry is in-bounds using sequential reads.
      // The corresponding page header is checked when that page is requested,
      // avoiding one random SD seek per page before first paint.
      const XtcError error = validatePageTableRecord(entries[index], &info);
      if (error != XtcError::OK) return error;
      if (firstPage == 0 && index == 0) {
        m_defaultWidth = info.width;
        m_defaultHeight = info.height;
      }
    }
    m_openPageIndex += entryCount;
    validated += entryCount;
  }
  return XtcError::OK;
}

XtcError XtcParser::validatePageEntry(const uint32_t pageIndex, PageInfo* info) {
  if (pageIndex >= m_header.pageCount) return XtcError::PAGE_OUT_OF_RANGE;
  if (!ensureFileOpen()) return XtcError::FILE_NOT_FOUND;

  const uint64_t entryOffset = m_header.pageTableOffset + static_cast<uint64_t>(pageIndex) * sizeof(PageTableEntry);
  if (!rangeWithin(entryOffset, sizeof(PageTableEntry), m_fileSize) || !m_file.seek64(entryOffset)) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  PageTableEntry entry{};
  if (m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) != sizeof(entry)) return XtcError::READ_ERROR;
  return validatePageRecord(entry, info);
}

XtcError XtcParser::validatePageTableRecord(const PageTableEntry& entry, PageInfo* info) const {
  if (entry.width != DISPLAY_WIDTH || entry.height != DISPLAY_HEIGHT) return XtcError::UNSUPPORTED_DIMENSIONS;
  if (entry.dataOffset < m_header.dataOffset || !rangeWithin(entry.dataOffset, entry.dataSize, m_fileSize) ||
      entry.dataSize < sizeof(XtgPageHeader)) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }

  PageLayout layout;
  if (!calculatePageLayout(entry.width, entry.height, m_bitDepth, layout)) return XtcError::SIZE_MISMATCH;
  size_t encodedBytes = 0;
  if (!checkedAdd(sizeof(XtgPageHeader), layout.payloadBytes, encodedBytes) || entry.dataSize != encodedBytes) {
    return XtcError::SIZE_MISMATCH;
  }

  if (info) {
    info->offset = entry.dataOffset;
    info->size = entry.dataSize;
    info->width = entry.width;
    info->height = entry.height;
    info->bitDepth = m_bitDepth;
    info->padding = 0;
  }
  return XtcError::OK;
}

XtcError XtcParser::validatePageRecord(const PageTableEntry& entry, PageInfo* info) {
  PageInfo tableInfo;
  const XtcError tableError = validatePageTableRecord(entry, &tableInfo);
  if (tableError != XtcError::OK) return tableError;
  if (!m_file.seek64(entry.dataOffset)) return XtcError::READ_ERROR;

  XtgPageHeader pageHeader{};
  if (m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(pageHeader)) != sizeof(pageHeader)) {
    return XtcError::READ_ERROR;
  }
  const uint32_t expectedMagic = m_bitDepth == 2 ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) return XtcError::INVALID_MAGIC;
  if (pageHeader.width != entry.width || pageHeader.height != entry.height) return XtcError::SIZE_MISMATCH;
  if (pageHeader.width != DISPLAY_WIDTH || pageHeader.height != DISPLAY_HEIGHT) {
    return XtcError::UNSUPPORTED_DIMENSIONS;
  }
  if (pageHeader.colorMode != 0 || pageHeader.compression != 0) return XtcError::UNSUPPORTED_COMPRESSION;

  PageLayout layout;
  if (!calculatePageLayout(pageHeader.width, pageHeader.height, m_bitDepth, layout) ||
      pageHeader.dataSize != layout.payloadBytes) {
    return XtcError::SIZE_MISMATCH;
  }

  if (info) *info = tableInfo;
  // Deliberately leave the cursor immediately after the page header so the
  // load methods can read the already-validated payload without a second seek.
  return XtcError::OK;
}

bool XtcParser::readPageTableEntry(const uint32_t pageIndex, PageInfo& info) {
  const XtcError error = validatePageEntry(pageIndex, &info);
  m_lastError = error;
  return error == XtcError::OK;
}

XtcError XtcParser::validateChaptersStep(const size_t maxChapters) {
  if (!m_hasChapters) {
    m_openChapterIndex = m_chapterCount;
    return XtcError::OK;
  }
  if (maxChapters == 0 || m_openChapterIndex > m_chapterCount || !ensureFileOpen()) {
    return XtcError::INVALID_ARGUMENT;
  }
  const uint64_t offset = m_header.chapterOffset + static_cast<uint64_t>(m_openChapterIndex) * XTC_CHAPTER_SIZE;
  if (!m_file.seek64(offset)) return XtcError::READ_ERROR;

  std::array<uint8_t, XTC_CHAPTER_SIZE> bytes{};
  size_t validated = 0;
  while (m_openChapterIndex < m_chapterCount && validated < maxChapters) {
    const int chapterRead = m_file.read(bytes.data(), bytes.size());
    if (chapterRead < 0 || static_cast<size_t>(chapterRead) != bytes.size()) return XtcError::READ_ERROR;

    uint16_t startPage = 0;
    uint16_t endPage = 0;
    // The supported converter writes chapter page numbers as 1-based values.
    if (!readValidChapterRange(bytes, m_header.pageCount, startPage, endPage)) {
      return XtcError::INVALID_CHAPTERS;
    }
    ++m_openChapterIndex;
    ++validated;
  }
  return XtcError::OK;
}

XtcError XtcParser::readChapters() {
  m_chapters.clear();
  if (!m_hasChapters) return XtcError::OK;
  if (!ensureFileOpen() || !m_file.seek64(m_header.chapterOffset)) return XtcError::READ_ERROR;

  m_chapters.reserve(m_chapterCount);
  std::array<uint8_t, XTC_CHAPTER_SIZE> bytes{};
  for (uint16_t index = 0; index < m_chapterCount; ++index) {
    const int chapterRead = m_file.read(bytes.data(), bytes.size());
    if (chapterRead < 0 || static_cast<size_t>(chapterRead) != bytes.size()) return XtcError::READ_ERROR;

    uint16_t startPage = 0;
    uint16_t endPage = 0;
    if (!readValidChapterRange(bytes, m_header.pageCount, startPage, endPage)) {
      return XtcError::INVALID_CHAPTERS;
    }
    std::string name(reinterpret_cast<const char*>(bytes.data()),
                     strnlen(reinterpret_cast<const char*>(bytes.data()), 80));
    m_chapters.push_back({std::move(name), static_cast<uint16_t>(startPage - 1U), static_cast<uint16_t>(endPage - 1U)});
  }
  return XtcError::OK;
}

const std::vector<ChapterInfo>& XtcParser::getChapters() {
  if (!m_chaptersLoaded && m_hasChapters) {
    const XtcError error = readChapters();
    if (error != XtcError::OK) {
      LOG_ERR("XTC", "Failed to load chapters: %s", errorToString(error));
      m_chapters.clear();
      m_hasChapters = false;
      m_lastError = error;
    }
    m_chaptersLoaded = true;
    closeFile();
  }
  return m_chapters;
}

bool XtcParser::getPageInfo(const uint32_t pageIndex, PageInfo& info) {
  const bool ok = readPageTableEntry(pageIndex, info);
  closeFile();
  return ok;
}

bool XtcParser::openPagePayloadStream(const uint32_t pageIndex, PageInfo& info, HalFile& file) {
  if (file.isOpen()) {
    m_lastError = XtcError::INVALID_ARGUMENT;
    return false;
  }
  m_lastError = validatePageEntry(pageIndex, &info);
  if (m_lastError != XtcError::OK) {
    closeFile();
    return false;
  }
  file = std::move(m_file);
  return file.isOpen();
}

size_t XtcParser::loadPage(const uint32_t pageIndex, uint8_t* buffer, const size_t bufferSize) {
  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }
  if (!buffer) {
    m_lastError = XtcError::INVALID_ARGUMENT;
    return 0;
  }

  PageInfo page;
  m_lastError = validatePageEntry(pageIndex, &page);
  if (m_lastError != XtcError::OK) {
    closeFile();
    return 0;
  }
  PageLayout layout;
  if (!calculatePageLayout(page.width, page.height, page.bitDepth, layout) || bufferSize < layout.payloadBytes) {
    m_lastError = XtcError::MEMORY_ERROR;
    closeFile();
    return 0;
  }
  const size_t read = m_file.read(buffer, layout.payloadBytes);
  const bool sizeStable = m_file.fileSize64() == m_fileSize;
  closeFile();
  if (read != layout.payloadBytes || !sizeStable) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }
  m_lastError = XtcError::OK;
  return read;
}

XtcError XtcParser::loadPageStreaming(const uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      const size_t chunkSize) {
  if (!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if (!callback || chunkSize == 0) return XtcError::INVALID_ARGUMENT;

  PageInfo page;
  XtcError error = validatePageEntry(pageIndex, &page);
  if (error != XtcError::OK) {
    closeFile();
    return error;
  }
  PageLayout layout;
  if (!calculatePageLayout(page.width, page.height, page.bitDepth, layout)) {
    closeFile();
    return XtcError::SIZE_MISMATCH;
  }

  std::array<uint8_t, 1024> chunk;
  const size_t boundedChunkSize = std::min(chunkSize, chunk.size());
  size_t totalRead = 0;
  while (totalRead < layout.payloadBytes) {
    const size_t wanted = std::min(boundedChunkSize, layout.payloadBytes - totalRead);
    const int read = m_file.read(chunk.data(), wanted);
    if (read <= 0 || static_cast<size_t>(read) > wanted) {
      closeFile();
      return XtcError::READ_ERROR;
    }
    callback(chunk.data(), static_cast<size_t>(read), totalRead);
    totalRead += static_cast<size_t>(read);
  }
  const bool sizeStable = m_file.fileSize64() == m_fileSize;
  closeFile();
  return sizeStable ? XtcError::OK : XtcError::READ_ERROR;
}

XtcError XtcParser::loadXthPlanePairs(
    const uint32_t pageIndex,
    std::function<void(uint8_t* bit0, uint8_t* bit1, size_t size, size_t planeOffset)> callback,
    const size_t chunkSize) {
  if (!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if (!callback || chunkSize == 0 || m_bitDepth != 2) return XtcError::INVALID_ARGUMENT;

  PageInfo page;
  XtcError error = validatePageEntry(pageIndex, &page);
  if (error != XtcError::OK) {
    closeFile();
    return error;
  }
  PageLayout layout;
  if (!calculatePageLayout(page.width, page.height, page.bitDepth, layout) || layout.columnBytes == 0) {
    closeFile();
    return XtcError::SIZE_MISMATCH;
  }

  constexpr size_t MAX_PLANE_CHUNK = 1024;
  size_t boundedChunkSize = std::min(chunkSize, MAX_PLANE_CHUNK);
  boundedChunkSize -= boundedChunkSize % layout.columnBytes;
  if (boundedChunkSize == 0) {
    if (layout.columnBytes > MAX_PLANE_CHUNK) {
      closeFile();
      return XtcError::UNSUPPORTED_DIMENSIONS;
    }
    boundedChunkSize = layout.columnBytes;
  }
  auto chunks = makeUniqueNoThrow<uint8_t[]>(boundedChunkSize * 2U);
  if (!chunks) {
    closeFile();
    return XtcError::MEMORY_ERROR;
  }

  HalFile secondPlaneFile;
  if (page.offset > std::numeric_limits<uint64_t>::max() - sizeof(XtgPageHeader)) {
    closeFile();
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  const uint64_t payloadOffset = page.offset + sizeof(XtgPageHeader);
  if (layout.planeBytes > std::numeric_limits<uint64_t>::max() - payloadOffset) {
    closeFile();
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  const uint64_t secondPlaneOffset = payloadOffset + layout.planeBytes;
  if (!Storage.openFileForRead("XTC", m_filepath.c_str(), secondPlaneFile) ||
      !secondPlaneFile.seek64(secondPlaneOffset)) {
    closeFile();
    return XtcError::READ_ERROR;
  }

  uint8_t* const first = chunks.get();
  uint8_t* const second = chunks.get() + boundedChunkSize;
  size_t planeRead = 0;
  while (planeRead < layout.planeBytes) {
    const size_t wanted = std::min(boundedChunkSize, layout.planeBytes - planeRead);
    const int firstRead = m_file.read(first, wanted);
    const int secondRead = secondPlaneFile.read(second, wanted);
    if (firstRead <= 0 || secondRead <= 0 || static_cast<size_t>(firstRead) != wanted ||
        static_cast<size_t>(secondRead) != wanted) {
      secondPlaneFile.close();
      closeFile();
      return XtcError::READ_ERROR;
    }
    callback(first, second, wanted, planeRead);
    planeRead += wanted;
  }

  const bool sizeStable = m_file.fileSize64() == m_fileSize && secondPlaneFile.fileSize64() == m_fileSize;
  const bool secondPlaneClosed = secondPlaneFile.close();
  closeFile();
  return sizeStable && secondPlaneClosed ? XtcError::OK : XtcError::READ_ERROR;
}

XtcError XtcParser::fingerprintSourceStep(const size_t maxBytes) {
  if (!m_file.isOpen() || maxBytes == 0 || m_openFingerprintBytes > m_fileSize) return XtcError::INVALID_ARGUMENT;
  std::array<uint8_t, IDENTITY_CHUNK_SIZE> buffer;
  size_t bytesThisStep = 0;
  while (m_openFingerprintBytes < m_fileSize && bytesThisStep < maxBytes) {
    const size_t wanted = static_cast<size_t>(
        std::min<uint64_t>({buffer.size(), m_fileSize - m_openFingerprintBytes, maxBytes - bytesThisStep}));
    const int read = m_file.read(buffer.data(), wanted);
    if (read <= 0 || static_cast<size_t>(read) > wanted) return XtcError::READ_ERROR;
    m_openFingerprint.update(buffer.data(), static_cast<size_t>(read));
    m_openFingerprintBytes += static_cast<size_t>(read);
    bytesThisStep += static_cast<size_t>(read);
  }
  if (m_openFingerprintBytes < m_fileSize) return XtcError::OK;
  if (m_file.fileSize64() != m_fileSize) return XtcError::READ_ERROR;
  m_sourceIdentity = m_openFingerprint.finish(m_fileSize);
  m_sourceIdentityHandoff = {};
  m_sourceIdentityHandoff.capture(m_filepath, m_file, m_sourceIdentity);
  m_hasSourceIdentity = true;
  m_reusedSourceIdentity = false;
  return XtcError::OK;
}

bool XtcParser::getSourceIdentity(ZipFile::SourceIdentity& identity) const {
  if (!m_isOpen || !m_hasSourceIdentity || !m_sourceIdentity.isRawFile()) return false;
  identity = m_sourceIdentity;
  return true;
}

bool XtcParser::getSourceIdentityHandoff(RawSourceIdentityHandoff& handoff) const {
  if (!m_isOpen || !m_hasSourceIdentity || !m_sourceIdentityHandoff.valid ||
      m_sourceIdentityHandoff.identity != m_sourceIdentity) {
    return false;
  }
  handoff = m_sourceIdentityHandoff;
  return true;
}

}  // namespace xtc
