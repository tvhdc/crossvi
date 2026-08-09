#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "ProgressFileCodec.h"

namespace ProgressFile {

enum class LoadSource : uint8_t { Missing, Primary, Backup, Temp, Invalid, IoError };

struct LoadResult {
  LoadSource source = LoadSource::Missing;
  size_t size = 0;

  explicit operator bool() const {
    return source == LoadSource::Primary || source == LoadSource::Backup || source == LoadSource::Temp;
  }
};

using ValidatorFunction = bool (*)(const uint8_t* data, size_t size, const void* context);

// Optional semantic validation applied independently to primary, backup and
// temp candidates. Size checks alone cannot distinguish a complete but
// nonsensical progress record from a usable one.
struct CandidateValidator {
  ValidatorFunction function = nullptr;
  const void* context = nullptr;

  bool accepts(const uint8_t* data, const size_t size) const {
    return function == nullptr || function(data, size, context);
  }
};

// Optional write-time guard for a recognized record owned by newer firmware.
// Unlike semantic validation, a protected candidate must never be replaced by
// a fallback or a newly generated record.
struct CandidateProtector {
  ValidatorFunction function = nullptr;
  const void* context = nullptr;

  bool protects(const uint8_t* data, const size_t size) const {
    return function != nullptr && function(data, size, context);
  }
};

struct EpubBounds {
  uint32_t spineCount = 0;
};

struct PageBounds {
  uint32_t pageCount = 0;
};

struct TxtBounds {
  uint32_t fileSize = 0;
  uint32_t legacyPageCount = 0;
};

constexpr size_t EPUB_LEGACY_PROGRESS_SIZE = 4;
constexpr size_t EPUB_PROGRESS_SIZE = 6;
// The content-anchored layout appends a uint32 visible-text offset to the
// shared six-byte position. Older four- and six-byte records remain readable.
constexpr size_t EPUB_CONTENT_ANCHORED_PROGRESS_SIZE = 10;

inline bool validateEpubBounds(const uint8_t* data, const size_t size, const void* context) {
  if (!data ||
      (size != EPUB_LEGACY_PROGRESS_SIZE && size != EPUB_PROGRESS_SIZE &&
       size != EPUB_CONTENT_ANCHORED_PROGRESS_SIZE) ||
      !context) {
    return false;
  }
  const auto& bounds = *static_cast<const EpubBounds*>(context);
  if (bounds.spineCount == 0) return false;

  const uint16_t spineIndex = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  const uint16_t pageNumber = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
  if (spineIndex >= bounds.spineCount || pageNumber == UINT16_MAX) return false;

  if (size >= EPUB_PROGRESS_SIZE) {
    const uint16_t pageCount = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    // A zero count is intentionally supported for legacy/footnote save paths
    // that know the exact resume page but do not yet know the chapter total.
    if (pageCount > 0 && pageNumber >= pageCount) return false;
  }
  return true;
}

inline bool validatePageBounds(const uint8_t* data, const size_t size, const void* context) {
  if (!data || size != 4 || !context) return false;
  const auto& bounds = *static_cast<const PageBounds*>(context);
  if (bounds.pageCount == 0) return false;

  const uint32_t page = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return page < bounds.pageCount;
}

inline bool validateTxtBounds(const uint8_t* data, const size_t size, const void* context) {
  if (!data || !context) return false;
  const auto& bounds = *static_cast<const TxtBounds*>(context);
  uint32_t value = 0;
  switch (ProgressFileCodec::decodeTxt(data, size, value)) {
    case ProgressFileCodec::TxtDecodeStatus::Ok:
      return bounds.fileSize > 0 && value < bounds.fileSize;
    case ProgressFileCodec::TxtDecodeStatus::LegacyPage:
      return bounds.legacyPageCount > 0 && value < bounds.legacyPageCount;
    default:
      return false;
  }
}

namespace detail {

enum class CandidateStatus : uint8_t { Missing, Valid, Invalid, Protected, IoError };

struct CandidateResult {
  CandidateStatus status = CandidateStatus::Missing;
  size_t size = 0;
};

inline bool acceptsSize(const size_t size, const size_t* acceptedSizes, const size_t acceptedSizeCount) {
  for (size_t i = 0; i < acceptedSizeCount; ++i) {
    if (size == acceptedSizes[i]) return true;
  }
  return false;
}

inline CandidateResult readCandidate(const std::string& path, uint8_t* data, const size_t capacity,
                                     const size_t* acceptedSizes, const size_t acceptedSizeCount,
                                     const CandidateValidator validator = {}, const CandidateProtector protector = {}) {
  if (!Storage.exists(path.c_str())) return {};

  HalFile file;
  if (!Storage.openFileForRead("PRG", path, file)) return {CandidateStatus::IoError, 0};

  const size_t size = file.fileSize();
  const bool acceptedSize = acceptsSize(size, acceptedSizes, acceptedSizeCount);
  if (size > capacity || (!acceptedSize && protector.function == nullptr)) {
    return {file.close() ? CandidateStatus::Invalid : CandidateStatus::IoError, size};
  }
  if (size > 0 && file.read(data, size) != static_cast<int>(size)) {
    file.close();
    return {CandidateStatus::IoError, size};
  }
  if (!file.close()) return {CandidateStatus::IoError, size};
  if (protector.protects(data, size)) return {CandidateStatus::Protected, size};
  if (!acceptedSize) return {CandidateStatus::Invalid, size};
  return {validator.accepts(data, size) ? CandidateStatus::Valid : CandidateStatus::Invalid, size};
}

inline LoadResult load(const std::string& cachePath, uint8_t* data, const size_t capacity, const size_t* acceptedSizes,
                       const size_t acceptedSizeCount, const CandidateValidator validator = {}) {
  if (!data || capacity == 0 || !acceptedSizes || acceptedSizeCount == 0) return {LoadSource::Invalid, 0};

  const std::string primaryPath = cachePath + "/progress.bin";
  const std::string backupPath = primaryPath + ".bak";
  const std::string tempPath = primaryPath + ".tmp";
  const std::string* paths[] = {&primaryPath, &backupPath, &tempPath};
  constexpr LoadSource sources[] = {LoadSource::Primary, LoadSource::Backup, LoadSource::Temp};
  bool sawInvalid = false;
  bool sawIoError = false;

  for (size_t i = 0; i < 3; ++i) {
    const CandidateResult candidate =
        readCandidate(*paths[i], data, capacity, acceptedSizes, acceptedSizeCount, validator);
    if (candidate.status == CandidateStatus::Valid) return {sources[i], candidate.size};
    sawInvalid = sawInvalid || candidate.status == CandidateStatus::Invalid;
    sawIoError = sawIoError || candidate.status == CandidateStatus::IoError;
  }
  if (sawIoError) return {LoadSource::IoError, 0};
  return {sawInvalid ? LoadSource::Invalid : LoadSource::Missing, 0};
}

inline bool verifyExact(const std::string& path, const uint8_t* expected, const size_t size) {
  uint8_t actual[EPUB_CONTENT_ANCHORED_PROGRESS_SIZE]{};
  const size_t acceptedSize = size;
  const CandidateResult result = readCandidate(path, actual, sizeof(actual), &acceptedSize, 1);
  return result.status == CandidateStatus::Valid && memcmp(actual, expected, size) == 0;
}

inline bool writeVerified(const std::string& path, const uint8_t* data, const size_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("PRG", path, file)) return false;
  if (file.write(data, size) != size) {
    file.close();
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  return synced && closed && verifyExact(path, data, size);
}

inline bool removeIfPresent(const std::string& path) {
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}

struct TxtValidatorContext {
  CandidateValidator downstream;
};

inline bool validateTxtRecord(const uint8_t* data, const size_t size, const void* rawContext) {
  if (!rawContext) return false;
  uint32_t ignored = 0;
  const ProgressFileCodec::TxtDecodeStatus decoded = ProgressFileCodec::decodeTxt(data, size, ignored);
  if (decoded != ProgressFileCodec::TxtDecodeStatus::Ok && decoded != ProgressFileCodec::TxtDecodeStatus::LegacyPage) {
    return false;
  }
  return static_cast<const TxtValidatorContext*>(rawContext)->downstream.accepts(data, size);
}

inline bool protectsFutureTxtRecord(const uint8_t* data, const size_t size, const void*) {
  return data && size >= 2 && size != 4 && data[0] == ProgressFileCodec::TXT_MAGIC &&
         data[1] > ProgressFileCodec::TXT_VERSION;
}

enum class TxtVersionStatus : uint8_t { Compatible, NewerVersion, IoError };

inline TxtVersionStatus inspectTxtVersion(const std::string& path) {
  if (!Storage.exists(path.c_str())) return TxtVersionStatus::Compatible;

  HalFile file;
  if (!Storage.openFileForRead("PRG", path, file)) return TxtVersionStatus::IoError;
  const size_t size = file.fileSize();
  uint8_t prefix[2]{};
  // Four bytes are unconditionally the legacy uint32 page layout. Values such
  // as page 852 encode to {'T', 3, 0, 0} and must not be mistaken for a future
  // versioned envelope merely because their low bytes resemble its prefix.
  const bool hasVersionPrefix = size >= sizeof(prefix) && size != 4;
  const bool prefixRead = !hasVersionPrefix || file.read(prefix, sizeof(prefix)) == 2;
  const bool closed = file.close();
  if (!prefixRead || !closed) return TxtVersionStatus::IoError;
  if (hasVersionPrefix && prefix[0] == ProgressFileCodec::TXT_MAGIC && prefix[1] > ProgressFileCodec::TXT_VERSION) {
    return TxtVersionStatus::NewerVersion;
  }
  return TxtVersionStatus::Compatible;
}

inline std::array<std::string, 3> txtCandidatePaths(const std::string& cachePath) {
  return {cachePath + "/progress.bin", cachePath + "/progress.bin.bak", cachePath + "/progress.bin.tmp"};
}

inline bool txtCandidatesAllowLoad(const std::string& cachePath) {
  const auto paths = txtCandidatePaths(cachePath);
  const auto newer = std::find_if(paths.begin(), paths.end(), [](const std::string& path) {
    return inspectTxtVersion(path) == TxtVersionStatus::NewerVersion;
  });
  if (newer != paths.end()) {
    LOG_ERR("PRG", "Refusing to load alongside newer TXT progress: %s", newer->c_str());
    return false;
  }
  return true;
}

}  // namespace detail

// Prefer content-anchored EPUB progress while retaining both older layouts.
inline LoadResult loadEpub(const std::string& cachePath, uint8_t* data, const size_t capacity,
                           const CandidateValidator validator = {}) {
  constexpr size_t ACCEPTED_SIZES[] = {EPUB_CONTENT_ANCHORED_PROGRESS_SIZE, EPUB_PROGRESS_SIZE,
                                       EPUB_LEGACY_PROGRESS_SIZE};
  return detail::load(cachePath, data, capacity, ACCEPTED_SIZES, 3, validator);
}

// TXT v2 stores a layout-independent byte offset. Four-byte legacy page
// records remain readable so the caller can map and republish them after the
// new page index has been built.
inline LoadResult loadTxt(const std::string& cachePath, uint8_t* data, const size_t capacity,
                          const CandidateValidator validator = {}) {
  // A valid older backup must not hide a newer primary/temp (or vice versa).
  // Resuming that stale copy could later overwrite progress owned by firmware
  // whose layout this build does not understand.
  if (!detail::txtCandidatesAllowLoad(cachePath)) return {LoadSource::Invalid, 0};
  constexpr size_t ACCEPTED_SIZES[] = {ProgressFileCodec::TXT_V2_SIZE, 4};
  const detail::TxtValidatorContext context{validator};
  const CandidateValidator formatValidator{detail::validateTxtRecord, &context};
  return detail::load(cachePath, data, capacity, ACCEPTED_SIZES, 2, formatValidator);
}

// XTC and legacy TXT share the original four-byte little-endian page layout.
inline LoadResult loadPage(const std::string& cachePath, uint8_t* data, const size_t capacity,
                           const CandidateValidator validator = {}) {
  constexpr size_t ACCEPTED_SIZE = 4;
  return detail::load(cachePath, data, capacity, &ACCEPTED_SIZE, 1, validator);
}

// Writes a recognized progress layout through progress.bin.tmp,
// keeps the previous committed file as progress.bin.bak, and verifies bytes
// before and after publication. Readers fall back to backup and then temp, so
// every interruption point retains at least one usable copy on a healthy FAT.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, const size_t len,
                        const CandidateValidator validator = {}, const CandidateProtector protector = {},
                        const size_t compatibleExistingSize = 0) {
  if (!data || (len != 4 && len != 6 && len != EPUB_CONTENT_ANCHORED_PROGRESS_SIZE) || !validator.accepts(data, len)) {
    return false;
  }

  const std::string primaryPath = cachePath + "/progress.bin";
  const std::string backupPath = primaryPath + ".bak";
  const std::string tempPath = primaryPath + ".tmp";
  size_t acceptedSizes[3] = {len, 0, 0};
  size_t acceptedSizeCount = 1;
  const auto addAcceptedSize = [&](const size_t size) {
    if (size == 0) return;
    for (size_t i = 0; i < acceptedSizeCount; ++i) {
      if (acceptedSizes[i] == size) return;
    }
    acceptedSizes[acceptedSizeCount++] = size;
  };
  addAcceptedSize(EPUB_LEGACY_PROGRESS_SIZE);
  addAcceptedSize(compatibleExistingSize);
  uint8_t scratch[EPUB_CONTENT_ANCHORED_PROGRESS_SIZE]{};
  auto primary = detail::readCandidate(primaryPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount,
                                       validator, protector);
  const auto backup = detail::readCandidate(backupPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount,
                                            validator, protector);
  const auto temp =
      detail::readCandidate(tempPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount, validator, protector);
  const std::array<detail::CandidateResult, 3> candidates = {primary, backup, temp};

  // The canonical primary is authoritative once it has been read and
  // validated. Corrupt/unreadable backup and temp files are then stale
  // recovery artefacts: the normal remove/rotate steps below can heal them
  // without risking the committed primary. If the primary is not valid, keep
  // every unreadable or unknown candidate because it may be the only usable
  // progress copy. A recognized newer-version record is always protected,
  // regardless of which recovery path contains it.
  const auto isUnreadableOrUnknown = [len](const auto& candidate) {
    return candidate.status == detail::CandidateStatus::IoError ||
           (candidate.status == detail::CandidateStatus::Invalid && candidate.size > len);
  };
  const bool hasProtectedRecord = std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
    return candidate.status == detail::CandidateStatus::Protected;
  });
  const bool primaryIsValid = primary.status == detail::CandidateStatus::Valid;
  const bool hasUnrecoverableState =
      isUnreadableOrUnknown(primary) ||
      (!primaryIsValid && (isUnreadableOrUnknown(backup) || isUnreadableOrUnknown(temp)));
  if (hasProtectedRecord || hasUnrecoverableState) {
    LOG_ERR("PRG", "Refusing progress write: primary=%u/%u backup=%u/%u temp=%u/%u",
            static_cast<unsigned>(primary.status), static_cast<unsigned>(primary.size),
            static_cast<unsigned>(backup.status), static_cast<unsigned>(backup.size),
            static_cast<unsigned>(temp.status), static_cast<unsigned>(temp.size));
    return false;
  }

  // If a first-ever save was fully written but power failed before publication,
  // promote it before reusing the temp path. Otherwise a committed copy already
  // protects us and the old temp is stale.
  if (temp.status == detail::CandidateStatus::Valid && primary.status != detail::CandidateStatus::Valid &&
      backup.status != detail::CandidateStatus::Valid) {
    if ((primary.status == detail::CandidateStatus::Invalid && !Storage.remove(primaryPath.c_str())) ||
        !Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
      LOG_ERR("PRG", "Could not recover the only valid progress copy");
      return false;
    }
    primary = temp;
  } else if (!detail::removeIfPresent(tempPath)) {
    LOG_ERR("PRG", "Could not clear stale temp progress file: %s", tempPath.c_str());
    return false;
  }

  if (!detail::writeVerified(tempPath, data, len)) {
    LOG_ERR("PRG", "Could not fully write, sync, and verify temp progress file: %s", tempPath.c_str());
    detail::removeIfPresent(tempPath);
    return false;
  }

  bool rotated = false;
  if (primary.status == detail::CandidateStatus::Valid) {
    if (!detail::removeIfPresent(backupPath) || !Storage.rename(primaryPath.c_str(), backupPath.c_str())) {
      LOG_ERR("PRG", "Could not rotate progress backup: %s", primaryPath.c_str());
      detail::removeIfPresent(tempPath);
      return false;
    }
    rotated = true;
  } else if (!detail::removeIfPresent(primaryPath)) {
    LOG_ERR("PRG", "Could not replace invalid progress file: %s", primaryPath.c_str());
    detail::removeIfPresent(tempPath);
    return false;
  }

  if (!Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
    LOG_ERR("PRG", "Failed to publish temp progress file: %s", primaryPath.c_str());
    if (rotated && !Storage.rename(backupPath.c_str(), primaryPath.c_str())) {
      LOG_ERR("PRG", "Progress rollback remains available in backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (!detail::verifyExact(primaryPath, data, len)) {
    LOG_ERR("PRG", "Published progress verification failed: %s", primaryPath.c_str());
    // The caller's bytes are still resident. Recreate and verify temp before
    // removing a bad first-ever primary, otherwise this recovery path itself
    // could turn a media error into complete progress loss.
    const bool tempRecovered = detail::writeVerified(tempPath, data, len);
    if (!tempRecovered) LOG_ERR("PRG", "Could not preserve failed publication in temp: %s", tempPath.c_str());
    if (rotated || tempRecovered) {
      if (!detail::removeIfPresent(primaryPath)) {
        LOG_ERR("PRG", "Could not remove failed progress publication: %s", primaryPath.c_str());
      } else if (rotated && !Storage.rename(backupPath.c_str(), primaryPath.c_str())) {
        LOG_ERR("PRG", "Progress rollback remains available in backup: %s", backupPath.c_str());
      }
    }
    return false;
  }
  return true;
}

// Writes either the legacy six-byte EPUB position or the content-anchored
// ten-byte position while accepting the other compatible layout as the
// previous committed copy.
inline bool writeEpubAtomic(const std::string& cachePath, const uint8_t* data, const size_t len,
                            const CandidateValidator validator = {}) {
  if (len != EPUB_PROGRESS_SIZE && len != EPUB_CONTENT_ANCHORED_PROGRESS_SIZE) return false;
  const size_t compatibleSize = len == EPUB_PROGRESS_SIZE ? EPUB_CONTENT_ANCHORED_PROGRESS_SIZE : EPUB_PROGRESS_SIZE;
  return writeAtomic(cachePath, data, len, validator, {}, compatibleSize);
}

inline bool writeTxtAtomic(const std::string& cachePath, const uint8_t (&data)[ProgressFileCodec::TXT_V2_SIZE],
                           const CandidateValidator validator = {}) {
  if (!detail::txtCandidatesAllowLoad(cachePath)) return false;
  uint32_t ignored = 0;
  if (ProgressFileCodec::decodeTxt(data, sizeof(data), ignored) != ProgressFileCodec::TxtDecodeStatus::Ok) {
    return false;
  }
  const detail::TxtValidatorContext context{validator};
  const CandidateValidator formatValidator{detail::validateTxtRecord, &context};
  const CandidateProtector futureProtector{detail::protectsFutureTxtRecord, nullptr};
  return writeAtomic(cachePath, data, sizeof(data), formatValidator, futureProtector);
}

}  // namespace ProgressFile
