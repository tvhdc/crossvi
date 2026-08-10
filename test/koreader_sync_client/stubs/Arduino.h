#pragma once

#include <WString.h>

#include <cstdint>

class EspStub {
 public:
  uint32_t getFreeHeap() const { return 1024U * 1024U; }
  uint32_t getMaxAllocHeap() const { return 1024U * 1024U; }
};

inline EspStub ESP;
inline uint32_t millis() { return 0; }
inline void yield() {}
