#pragma once

#include <cstddef>

namespace UploadPathGuard {

// Leaves enough room for the leading dot and the longest replacement sibling
// on FAT (255-byte long-file-name limit).
inline constexpr size_t MAX_LEAF_BYTES = 230;

bool isSafeLeafName(const char* name);
bool isSafeAbsolutePath(const char* path, bool allowRoot = true);
bool parseSize(const char* token, size_t& value);
bool isValidChunkRange(size_t offset, size_t total, size_t chunkSize, size_t maximumChunkSize);

}  // namespace UploadPathGuard
