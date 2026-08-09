#pragma once

#include <cstdint>

class EspStub {
 public:
  uint32_t getFreeHeap() const { return freeHeap_; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap_; }
  void setHeap(const uint32_t freeHeap, const uint32_t maxAllocHeap) {
    freeHeap_ = freeHeap;
    maxAllocHeap_ = maxAllocHeap;
  }

 private:
  uint32_t freeHeap_ = 1024U * 1024U;
  uint32_t maxAllocHeap_ = 1024U * 1024U;
};

inline EspStub ESP;
inline uint32_t millis() { return 0; }
inline void yield() {}
