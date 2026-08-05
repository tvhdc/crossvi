#pragma once

#include <cstdint>

namespace freeink {
enum class X3DisplayVerdict : uint8_t { Inconclusive, Uc8253Assumed, Uc8279Confirmed };

namespace XteinkDetectFake {
inline X3DisplayVerdict verdict = X3DisplayVerdict::Inconclusive;
inline uint32_t callCount = 0;
inline void reset() {
  verdict = X3DisplayVerdict::Inconclusive;
  callCount = 0;
}
}  // namespace XteinkDetectFake

inline X3DisplayVerdict detectX3DisplayController(uint8_t*, uint8_t*) {
  ++XteinkDetectFake::callCount;
  return XteinkDetectFake::verdict;
}
}  // namespace freeink
