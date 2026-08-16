/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossVi
 */

#pragma once

#include <HalStorage.h>
#include <RawSourceIdentity.h>
#include <ZipFile.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "XtcTypes.h"

namespace xtc {

/**
 * XTC File Parser
 *
 * Reads XTC files from SD card and extracts page data.
 * Designed for ESP32-C3's limited RAM (~380KB) using streaming.
 *
 * The source file is kept closed between reads to free heap for rendering.
 * It is reopened on-demand for page table lookups and bitmap data reads.
 */
class XtcParser {
 public:
  enum class OpenStepResult : uint8_t { InProgress, Opened, Error };

  XtcParser();
  ~XtcParser();

  // File open/close
  XtcError open(const char* filepath);
  XtcError beginOpen(const char* filepath, const RawSourceIdentityHandoff* preparedIdentity = nullptr);
  OpenStepResult stepOpen(size_t maxRecords, size_t maxFingerprintBytes);
  void cancelOpen();
  void close();
  bool isOpen() const { return m_isOpen; }

  // Read only the bounded header/metadata fields used by library listings.
  // This deliberately does not validate every page or fingerprint the source;
  // callers must use open() before exposing any path-keyed user state.
  static XtcError readCoreMetadata(const char* filepath, std::string& title, std::string& author);

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH
  bool getSourceIdentity(ZipFile::SourceIdentity& identity) const;
  bool getSourceIdentityHandoff(RawSourceIdentityHandoff& handoff) const;
  // Validates the page record and transfers the already-positioned payload
  // handle to a cooperative consumer. The parser reopens its own handle on
  // demand, so rendering can continue while that consumer spans loop ticks.
  bool openPagePayloadStream(uint32_t pageIndex, PageInfo& info, HalFile& file);

  // Page information
  bool getPageInfo(uint32_t pageIndex, PageInfo& info);

  /**
   * Load page bitmap (raw 1-bit data, skipping XTG header)
   *
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer (caller allocated)
   * @param bufferSize Buffer size
   * @return Number of bytes read on success, 0 on failure
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);

  /**
   * Streaming page load
   * Memory-efficient method that reads page data in chunks.
   *
   * @param pageIndex Page index
   * @param callback Callback function to receive data chunks
   * @param chunkSize Chunk size (default: 1024 bytes)
   * @return Error code
   */
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  /**
   * Stream matching chunks from both XTH bit planes. The callback receives
   * mutable bounded work buffers so native display code can compose output in
   * place without retaining a full plane.
   */
  XtcError loadXthPlanePairs(
      uint32_t pageIndex, std::function<void(uint8_t* bit0, uint8_t* bit1, size_t size, size_t planeOffset)> callback,
      size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  const std::vector<ChapterInfo>& getChapters();

  // Validation
  static bool isValidXtcFile(const char* filepath);

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  HalFile m_file;
  std::string m_filepath;
  bool m_isOpen;
  XtcHeader m_header;
  uint64_t m_fileSize;
  uint16_t m_chapterCount;
  ZipFile::SourceIdentity m_sourceIdentity;
  bool m_hasSourceIdentity;
  RawSourceIdentityHandoff m_sourceIdentityHandoff;
  bool m_reusedSourceIdentity = false;
  std::vector<ChapterInfo> m_chapters;
  std::string m_title;
  std::string m_author;
  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;  // 1 = XTC/XTG (1-bit), 2 = XTCH/XTH (2-bit)
  bool m_hasChapters;
  bool m_chaptersLoaded;
  XtcError m_lastError;
  enum class OpenPhase : uint8_t { Idle, PageTable, Chapters, Fingerprint };
  OpenPhase m_openPhase = OpenPhase::Idle;
  uint32_t m_openPageIndex = 0;
  uint16_t m_openChapterIndex = 0;
  uint64_t m_openFingerprintBytes = 0;
  RawSourceIdentityAccumulator m_openFingerprint;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  uint32_t m_openStartedMs = 0;
  uint32_t m_validationFinishedMs = 0;
  uint32_t m_openStartFreeHeap = 0;
#endif

  // Internal helper functions
  XtcError readHeader();
  XtcError readMetadata();
  XtcError readChapters();
  XtcError validatePageTableStep(size_t maxEntries);
  XtcError validateChaptersStep(size_t maxChapters);
  XtcError validatePageEntry(uint32_t pageIndex, PageInfo* info = nullptr);
  XtcError validatePageTableRecord(const PageTableEntry& entry, PageInfo* info = nullptr) const;
  XtcError validatePageRecord(const PageTableEntry& entry, PageInfo* info = nullptr);
  XtcError fingerprintSourceStep(size_t maxBytes);
  bool readPageTableEntry(uint32_t pageIndex, PageInfo& info);
  XtcError failOpen(XtcError error);
  OpenStepResult completeOpen();

  // File handle management — reopen on demand, close after use
  bool ensureFileOpen();
  void closeFile();
};

}  // namespace xtc
