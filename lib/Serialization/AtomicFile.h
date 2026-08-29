#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AtomicFile {

using Validator = bool (*)(const uint8_t* data, size_t size, void* context);

enum class LoadStatus : uint8_t { Primary, Backup, Temp, Missing, Invalid, Oversize, IoError };
enum class SaveStatus : uint8_t { Saved, Unchanged, Oversize, InvalidExistingState, IoError };

LoadStatus load(const char* path, std::string& data, size_t maxSize, Validator validator, void* context = nullptr);

// rotateIfUnchanged checkpoints the current value into both primary and backup.
SaveStatus save(const char* path, const uint8_t* data, size_t size, size_t maxSize, Validator validator,
                void* context = nullptr, bool rotateIfUnchanged = false);

inline SaveStatus save(const char* path, const std::string& data, size_t maxSize, Validator validator,
                       void* context = nullptr, const bool rotateIfUnchanged = false) {
  return save(path, reinterpret_cast<const uint8_t*>(data.data()), data.size(), maxSize, validator, context,
              rotateIfUnchanged);
}

}  // namespace AtomicFile
