#pragma once

#include <cstddef>
#include <cstdint>

namespace StagedFileTransaction {

using Validator = bool (*)(const char* path, void* context);

struct Digest {
  uint64_t size = 0;
  uint32_t hash = 2166136261U;

  bool operator==(const Digest& other) const { return size == other.size && hash == other.hash; }
};

enum class Status : uint8_t { Published, Recovered, NoRecoveryNeeded, InvalidStaging, IoError };

Status recover(const char* finalPath, const char* backupPath, Validator validator, void* context = nullptr);
Status publish(const char* finalPath, const char* stagingPath, const char* backupPath, Validator validator,
               void* context = nullptr);
void updateDigest(Digest& digest, const uint8_t* data, size_t size);
bool digestFile(const char* path, Digest& digest);

// Publish a staging file whose digest was accumulated while streaming it.
// The published file is validated and hashed exactly once before the backup is
// removed, so a failed rename, malformed file or byte mismatch can roll back.
Status publishAndVerify(const char* finalPath, const char* stagingPath, const char* backupPath,
                        const Digest& expectedDigest, Validator validator, void* context = nullptr);

// Rotate a staging file into place while retaining the previous final as a
// rollback candidate. The caller must validate the published bytes
// cooperatively, then call commitPendingPublish(); cancellation or validation
// failure must call rollbackPendingPublish().
Status beginPendingPublish(const char* finalPath, const char* stagingPath, const char* backupPath,
                           uint64_t expectedSize, Validator recoveryValidator, void* context = nullptr);
bool commitPendingPublish(const char* backupPath);
bool rollbackPendingPublish(const char* finalPath, const char* backupPath);

}  // namespace StagedFileTransaction
