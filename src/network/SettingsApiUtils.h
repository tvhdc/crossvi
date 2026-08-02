#pragma once

#include <cstddef>

namespace SettingsApiUtils {

constexpr bool isValidEnumIndex(const int index, const std::size_t optionCount) {
  return index >= 0 && static_cast<std::size_t>(index) < optionCount;
}

}  // namespace SettingsApiUtils
