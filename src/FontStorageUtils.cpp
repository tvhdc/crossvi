#include "FontStorageUtils.h"

#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace FontStorageUtils {
namespace {

bool formatPath(char* output, const size_t outputSize, const char* format, const char* first, const char* second,
                const char* third = nullptr) {
  if (!output || outputSize == 0 || !format || !first || !second) return false;
  const int written = third ? snprintf(output, outputSize, format, first, second, third)
                            : snprintf(output, outputSize, format, first, second);
  if (written < 0 || static_cast<size_t>(written) >= outputSize) {
    output[0] = '\0';
    return false;
  }
  return true;
}

bool removeDirectoryIfPresent(const char* path) { return !Storage.exists(path) || Storage.removeDir(path); }

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc;
}

}  // namespace

bool isValidFamilyName(const char* name) { return FontStorageContract::isValidFamilyName(name); }

bool isValidCpfontFilename(const char* name) { return FontStorageContract::isValidCpfontFilename(name); }

bool copyPersistedFamilyName(const char* name, char* output, const size_t outputSize) {
  if (!name || !output || outputSize == 0) return false;
  if (name[0] == '\0') {
    output[0] = '\0';
    return true;
  }
  const size_t length = strlen(name);
  if (!isValidFamilyName(name) || length >= outputSize) {
    output[0] = '\0';
    return false;
  }
  memcpy(output, name, length + 1);
  return true;
}

bool buildFamilyPath(const char* root, const char* family, char* output, const size_t outputSize) {
  if (!isValidFamilyName(family)) return false;
  return formatPath(output, outputSize, "%s/%s", root, family);
}

bool buildFontPath(const char* root, const char* family, const char* filename, char* output, const size_t outputSize) {
  if (!isValidFamilyName(family) || !isValidCpfontFilename(filename)) return false;
  return formatPath(output, outputSize, "%s/%s/%s", root, family, filename);
}

bool buildFilePath(const char* directory, const char* filename, char* output, const size_t outputSize) {
  if (!isValidCpfontFilename(filename)) return false;
  return formatPath(output, outputSize, "%s/%s", directory, filename);
}

bool buildTransactionDirectoryPath(const char* root, const char* family, const char* suffix, char* output,
                                   const size_t outputSize) {
  if (!isValidFamilyName(family) || !suffix || suffix[0] == '\0' || strchr(suffix, '/')) return false;
  return formatPath(output, outputSize, "%s/.%s%s", root, family, suffix);
}

bool computeFileCrc32(const char* path, uint32_t& crc, uint64_t& size) {
  HalFile file;
  if (!path || !Storage.openFileForRead("FONT", path, file)) return false;

  const uint64_t initialSize = file.fileSize64();
  uint64_t remaining = initialSize;
  uint32_t state = UINT32_MAX;
  std::array<uint8_t, 512> buffer;
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    if (file.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    state = updateCrc32(state, buffer.data(), chunk);
    remaining -= chunk;
  }
  if (file.fileSize64() != initialSize || !file.close()) return false;
  crc = ~state;
  size = initialSize;
  return true;
}

FileMatch fileMatches(const char* path, const uint64_t expectedSize, const uint32_t expectedCrc) {
  uint32_t actualCrc = 0;
  uint64_t actualSize = 0;
  if (!computeFileCrc32(path, actualCrc, actualSize)) return FileMatch::IoError;
  return actualSize == expectedSize && actualCrc == expectedCrc ? FileMatch::Match : FileMatch::Different;
}

bool discardStagingFamily(const char* stagingDirectory) {
  return stagingDirectory && removeDirectoryIfPresent(stagingDirectory);
}

FamilyTransactionStatus recoverFamily(const char* finalDirectory, const char* stagingDirectory,
                                      const char* backupDirectory, const FamilyValidator newFamilyValidator,
                                      void* newFamilyContext, const FamilyValidator oldFamilyValidator,
                                      void* oldFamilyContext) {
  if (!finalDirectory || !stagingDirectory || !backupDirectory || !newFamilyValidator || !oldFamilyValidator) {
    return FamilyTransactionStatus::IoError;
  }

  bool recovered = false;
  if (Storage.exists(backupDirectory)) {
    if (Storage.exists(finalDirectory) && newFamilyValidator(finalDirectory, newFamilyContext)) {
      if (!removeDirectoryIfPresent(backupDirectory)) return FamilyTransactionStatus::IoError;
    } else {
      if (!oldFamilyValidator(backupDirectory, oldFamilyContext)) return FamilyTransactionStatus::IoError;
      if (!removeDirectoryIfPresent(finalDirectory) || !Storage.rename(backupDirectory, finalDirectory)) {
        return FamilyTransactionStatus::IoError;
      }
      recovered = true;
    }
  }

  if (!discardStagingFamily(stagingDirectory)) return FamilyTransactionStatus::IoError;
  return recovered ? FamilyTransactionStatus::Recovered : FamilyTransactionStatus::NoRecoveryNeeded;
}

FamilyTransactionStatus publishFamily(const char* finalDirectory, const char* stagingDirectory,
                                      const char* backupDirectory, const FamilyValidator newFamilyValidator,
                                      void* newFamilyContext, const FamilyValidator oldFamilyValidator,
                                      void* oldFamilyContext) {
  if (!finalDirectory || !stagingDirectory || !backupDirectory || !newFamilyValidator || !oldFamilyValidator ||
      !Storage.exists(stagingDirectory) || !newFamilyValidator(stagingDirectory, newFamilyContext)) {
    return FamilyTransactionStatus::InvalidStaging;
  }
  // Recovery belongs before a new download begins. Refuse to overwrite a
  // surviving backup here because it is the only intact pre-update family.
  if (Storage.exists(backupDirectory)) return FamilyTransactionStatus::IoError;

  const bool hadOldFamily = Storage.exists(finalDirectory);
  if (hadOldFamily && !oldFamilyValidator(finalDirectory, oldFamilyContext)) {
    return FamilyTransactionStatus::IoError;
  }
  if (hadOldFamily && !Storage.rename(finalDirectory, backupDirectory)) {
    return FamilyTransactionStatus::IoError;
  }

  if (!Storage.rename(stagingDirectory, finalDirectory)) {
    if (hadOldFamily && !Storage.exists(finalDirectory)) Storage.rename(backupDirectory, finalDirectory);
    return FamilyTransactionStatus::IoError;
  }

  if (!newFamilyValidator(finalDirectory, newFamilyContext)) {
    removeDirectoryIfPresent(finalDirectory);
    if (hadOldFamily) Storage.rename(backupDirectory, finalDirectory);
    return FamilyTransactionStatus::IoError;
  }

  if (hadOldFamily && !removeDirectoryIfPresent(backupDirectory)) {
    // Both complete versions remain on disk. Recovery will keep the verified
    // new family and remove the backup on the next attempt.
    return FamilyTransactionStatus::IoError;
  }
  return FamilyTransactionStatus::Published;
}

}  // namespace FontStorageUtils
