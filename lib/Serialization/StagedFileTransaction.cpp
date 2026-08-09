#include "StagedFileTransaction.h"

#include <HalStorage.h>

#include <array>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif

namespace StagedFileTransaction {
namespace {

bool removeIfPresent(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

bool fileSizeMatches(const char* path, const uint64_t expectedSize) {
  HalFile file;
  if (!Storage.openFileForRead("STAGED", path, file)) return false;
  const bool matches = file.fileSize64() == expectedSize;
  return file.close() && matches;
}

Status rotateAndPublish(const char* finalPath, const char* stagingPath, const char* backupPath,
                        const uint64_t expectedSize, const bool keepBackup) {
  bool rotated = false;
  if (Storage.exists(finalPath)) {
    if (!removeIfPresent(backupPath) || !Storage.rename(finalPath, backupPath)) return Status::IoError;
    rotated = true;
  }

  if (!Storage.rename(stagingPath, finalPath)) {
    if (rotated && !Storage.exists(finalPath)) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }
  if (!fileSizeMatches(finalPath, expectedSize)) {
    removeIfPresent(finalPath);
    if (rotated) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }

  if (rotated && !keepBackup) removeIfPresent(backupPath);
  return Status::Published;
}

}  // namespace

void updateDigest(Digest& digest, const uint8_t* data, const size_t size) {
  if (!data || size == 0) return;
  for (size_t i = 0; i < size; ++i) {
    digest.hash ^= data[i];
    digest.hash *= 16777619U;
  }
  digest.size += size;
}

bool digestFile(const char* path, Digest& digest) {
  HalFile file;
  if (!Storage.openFileForRead("STAGED", path, file)) return false;
  digest = {0, 2166136261U};
  const uint64_t fileSize = file.fileSize64();
  std::array<uint8_t, 512> buffer{};
  uint64_t remaining = fileSize;
#if defined(ARDUINO_ARCH_ESP32)
  size_t bytesSinceYield = 0;
#endif
  while (remaining > 0) {
    const size_t chunk = remaining < buffer.size() ? static_cast<size_t>(remaining) : buffer.size();
    if (file.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    updateDigest(digest, buffer.data(), chunk);
    remaining -= chunk;
#if defined(ARDUINO_ARCH_ESP32)
    esp_task_wdt_reset();
    bytesSinceYield += chunk;
    if (bytesSinceYield >= 64U * 1024U) {
      yield();
      bytesSinceYield = 0;
    }
#endif
  }
  return digest.size == fileSize && file.close();
}

Status recover(const char* finalPath, const char* backupPath, const Validator validator, void* context) {
  if (!finalPath || !backupPath || !validator) return Status::IoError;
  if (!Storage.exists(backupPath)) return Status::NoRecoveryNeeded;

  if (Storage.exists(finalPath) && validator(finalPath, context)) {
    return removeIfPresent(backupPath) ? Status::NoRecoveryNeeded : Status::IoError;
  }
  // Do not remove an ambiguous final file unless the backup is already known
  // to be a complete recovery candidate. A transient read failure must not
  // turn an otherwise valid final into an invalid backup.
  if (!validator(backupPath, context)) {
    Digest readableBackup;
    if (Storage.exists(finalPath) || !digestFile(backupPath, readableBackup)) return Status::IoError;
    return Storage.remove(backupPath) ? Status::NoRecoveryNeeded : Status::IoError;
  }
  if (Storage.exists(finalPath) && !Storage.remove(finalPath)) return Status::IoError;
  if (!Storage.rename(backupPath, finalPath)) return Status::IoError;
  return validator(finalPath, context) ? Status::Recovered : Status::IoError;
}

Status publish(const char* finalPath, const char* stagingPath, const char* backupPath, const Validator validator,
               void* context) {
  if (!finalPath || !stagingPath || !backupPath || !validator || !Storage.exists(stagingPath) ||
      !validator(stagingPath, context)) {
    return Status::InvalidStaging;
  }
  Digest expected;
  if (!digestFile(stagingPath, expected)) return Status::IoError;

  const Status recovered = recover(finalPath, backupPath, validator, context);
  if (recovered == Status::IoError) return Status::IoError;

  const Status publishedStatus = rotateAndPublish(finalPath, stagingPath, backupPath, expected.size, true);
  if (publishedStatus != Status::Published) return publishedStatus;
  Digest published;
  if (!validator(finalPath, context) || !digestFile(finalPath, published) || !(published == expected)) {
    removeIfPresent(finalPath);
    if (Storage.exists(backupPath)) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }
  removeIfPresent(backupPath);
  return Status::Published;
}

Status publishAndVerify(const char* finalPath, const char* stagingPath, const char* backupPath,
                        const Digest& expectedDigest, const Validator validator, void* context) {
  if (!finalPath || !stagingPath || !backupPath || !validator || !Storage.exists(stagingPath) ||
      !fileSizeMatches(stagingPath, expectedDigest.size)) {
    return Status::InvalidStaging;
  }
  const Status recovered = recover(finalPath, backupPath, validator, context);
  if (recovered == Status::IoError) return Status::IoError;

  const Status publishedStatus = rotateAndPublish(finalPath, stagingPath, backupPath, expectedDigest.size, true);
  if (publishedStatus != Status::Published) return publishedStatus;

  Digest publishedDigest;
  if (!validator(finalPath, context)) {
    removeIfPresent(finalPath);
    if (Storage.exists(backupPath)) Storage.rename(backupPath, finalPath);
    return Status::InvalidStaging;
  }
  if (!digestFile(finalPath, publishedDigest) || !(publishedDigest == expectedDigest)) {
    removeIfPresent(finalPath);
    if (Storage.exists(backupPath)) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }
  removeIfPresent(backupPath);
  return Status::Published;
}

}  // namespace StagedFileTransaction
