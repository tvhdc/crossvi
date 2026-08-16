#pragma once

#include <Epub.h>
#include <Logging.h>

#include <optional>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt,
                         ProgressFile::WriteSession* writeSession = nullptr) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[ProgressFile::EPUB_CONTENT_ANCHORED_PROGRESS_SIZE];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  size_t dataSize = ProgressFile::EPUB_PROGRESS_SIZE;
  if (visibleTextOffset.has_value()) {
    data[6] = *visibleTextOffset & 0xFF;
    data[7] = (*visibleTextOffset >> 8) & 0xFF;
    data[8] = (*visibleTextOffset >> 16) & 0xFF;
    data[9] = (*visibleTextOffset >> 24) & 0xFF;
    dataSize = sizeof(data);
  }
  const int spineCount = epub.getSpineItemsCount();
  const ProgressFile::EpubBounds bounds{spineCount > 0 ? static_cast<uint32_t>(spineCount) : 0};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateEpubBounds, &bounds};
  const bool saved = writeSession ? writeSession->writeEpubAtomic(epub.getCachePath(), data, dataSize, validator)
                                  : ProgressFile::writeEpubAtomic(epub.getCachePath(), data, dataSize, validator);
  if (!saved) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d offset=%u page=%d", spineIndex, visibleTextOffset.value_or(0), pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
