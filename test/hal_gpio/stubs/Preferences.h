#pragma once

#include <cstdint>
#include <cstring>

namespace PreferencesFake {
inline uint8_t cachedDevice = 0;
inline void reset() { cachedDevice = 0; }
}  // namespace PreferencesFake

class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  uint8_t getUChar(const char* key, uint8_t defaultValue) {
    return std::strcmp(key, "dev_det") == 0 ? PreferencesFake::cachedDevice : defaultValue;
  }
  void putUChar(const char*, uint8_t) {}
  void end() {}
};
