#pragma once

#include <cstddef>

namespace SettingsApiUtils {

enum class PersistenceResult { Saved, DeviceFailed, KoReaderFailed };

constexpr bool isValidEnumIndex(const int index, const std::size_t optionCount) {
  return index >= 0 && static_cast<std::size_t>(index) < optionCount;
}

constexpr bool isValidToggle(const int value) { return value == 0 || value == 1; }

constexpr bool isValidValue(const int value, const std::size_t minimum, const std::size_t maximum) {
  return value >= 0 && static_cast<std::size_t>(value) >= minimum && static_cast<std::size_t>(value) <= maximum;
}

constexpr bool isValidStringLength(const std::size_t length, const std::size_t maximum) {
  return maximum == 0 || length <= maximum;
}

template <typename SaveDevice, typename SaveKoReader, typename RollbackDevice, typename RollbackKoReader>
PersistenceResult persistBatches(const bool deviceChanged, const bool koReaderChanged, SaveDevice&& saveDevice,
                                 SaveKoReader&& saveKoReader, RollbackDevice&& rollbackDevice,
                                 RollbackKoReader&& rollbackKoReader) {
  if (deviceChanged && !saveDevice()) {
    rollbackDevice();
    if (koReaderChanged) rollbackKoReader();
    return PersistenceResult::DeviceFailed;
  }
  if (koReaderChanged && !saveKoReader()) {
    rollbackKoReader();
    return PersistenceResult::KoReaderFailed;
  }
  return PersistenceResult::Saved;
}

}  // namespace SettingsApiUtils
