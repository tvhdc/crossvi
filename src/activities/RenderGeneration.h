#pragma once

#include <cstdint>

namespace RenderGeneration {

inline constexpr bool reached(const uint32_t completed, const uint32_t target) {
  return static_cast<int32_t>(completed - target) >= 0;
}

}  // namespace RenderGeneration
