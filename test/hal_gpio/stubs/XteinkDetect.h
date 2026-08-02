#pragma once

#include <cstdint>

namespace freeink {
enum class X3DisplayVerdict : uint8_t { Inconclusive, Uc8253Assumed, Uc8279Confirmed };

inline X3DisplayVerdict detectX3DisplayController(uint8_t*, uint8_t*) { return X3DisplayVerdict::Inconclusive; }
}  // namespace freeink
