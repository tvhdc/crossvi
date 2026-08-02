#pragma once

#include <cstdint>

namespace WireFake {
inline bool readsSucceed = true;
inline int16_t currentMa = 0;
inline uint8_t selectedRegister = 0;
inline uint8_t readIndex = 0;
inline uint32_t transactionCount = 0;

inline void reset() {
  readsSucceed = true;
  currentMa = 0;
  selectedRegister = 0;
  readIndex = 0;
  transactionCount = 0;
}
}  // namespace WireFake

class WireStub {
 public:
  void begin(int, int, uint32_t) {}
  void setTimeOut(uint16_t) {}
  void end() {}

  void beginTransmission(uint8_t) { ++WireFake::transactionCount; }
  void write(uint8_t value) { WireFake::selectedRegister = value; }
  uint8_t endTransmission(bool) { return WireFake::readsSucceed ? 0 : 1; }

  uint8_t requestFrom(uint8_t, uint8_t count, uint8_t) {
    WireFake::readIndex = 0;
    return WireFake::readsSucceed ? count : 0;
  }

  int available() const { return WireFake::readsSucceed ? 2 - WireFake::readIndex : 0; }

  uint8_t read() {
    const uint16_t raw = WireFake::selectedRegister == 0x0C ? static_cast<uint16_t>(WireFake::currentMa) : 0;
    return WireFake::readIndex++ == 0 ? static_cast<uint8_t>(raw & 0xFF) : static_cast<uint8_t>(raw >> 8);
  }
};

inline WireStub Wire;
