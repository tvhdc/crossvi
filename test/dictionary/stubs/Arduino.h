#pragma once

#include <cstdint>

constexpr int O_WRITE = 0x01;
constexpr int O_CREAT = 0x02;
constexpr int O_TRUNC = 0x04;

class EspStub {
 public:
  uint32_t getFreeHeap() const { return maxAllocHeap_; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap_; }
  void setMaxAllocHeap(const uint32_t bytes) { maxAllocHeap_ = bytes; }

 private:
  uint32_t maxAllocHeap_ = 1024U * 1024U;
};

inline EspStub ESP;
inline uint32_t millis() { return 0; }
inline void yield() {}
