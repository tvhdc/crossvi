#pragma once

#include <cstddef>
#include <cstring>

namespace FontStorageUtils {

inline bool copyPersistedFamilyName(const char* name, char* output, const size_t outputSize) {
  if (!name || !output || outputSize == 0) return false;
  const size_t length = std::strlen(name);
  if (length >= outputSize) {
    output[0] = '\0';
    return false;
  }
  std::memcpy(output, name, length + 1);
  return true;
}

}  // namespace FontStorageUtils
