#pragma once

#include <cstdint>
#include <cstring>

namespace PreferencesFake {
inline uint8_t cachedDevice = 0;
inline uint8_t deviceOverride = 0;
inline uint8_t epdCached = 0;
inline uint8_t epdOverride = 0;
inline void reset() {
  cachedDevice = 0;
  deviceOverride = 0;
  epdCached = 0;
  epdOverride = 0;
}
}  // namespace PreferencesFake

class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  uint8_t getUChar(const char* key, uint8_t defaultValue) {
    if (std::strcmp(key, "dev_det") == 0) return PreferencesFake::cachedDevice;
    if (std::strcmp(key, "dev_ovr") == 0) return PreferencesFake::deviceOverride;
    if (std::strcmp(key, "epd_det") == 0) return PreferencesFake::epdCached;
    if (std::strcmp(key, "epd_ovr") == 0) return PreferencesFake::epdOverride;
    return defaultValue;
  }
  void putUChar(const char*, uint8_t) {}
  void end() {}
};
