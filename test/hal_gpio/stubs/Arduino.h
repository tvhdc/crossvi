#pragma once

#include <cstdint>

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT = 0;

namespace ArduinoFake {
inline uint32_t now = 0;
inline int digitalValue = LOW;
inline uint32_t digitalReadCount = 0;
inline void (*delayHook)() = nullptr;

inline void reset() {
  now = 0;
  digitalValue = LOW;
  digitalReadCount = 0;
  delayHook = nullptr;
}
}  // namespace ArduinoFake

inline unsigned long millis() { return ArduinoFake::now; }

inline void delay(unsigned long durationMs) {
  ArduinoFake::now += static_cast<uint32_t>(durationMs);
  if (ArduinoFake::delayHook != nullptr) {
    ArduinoFake::delayHook();
  }
}

inline void pinMode(int, int) {}

inline int digitalRead(int) {
  ++ArduinoFake::digitalReadCount;
  return ArduinoFake::digitalValue;
}
