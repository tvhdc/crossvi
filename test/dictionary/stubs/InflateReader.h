#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace InflateReaderStub {
inline bool initSucceeds = true;
inline bool readSucceeds = true;
inline bool callbackSet = false;

inline void reset() {
  initSucceeds = true;
  readSucceeds = true;
  callbackSet = false;
}
}  // namespace InflateReaderStub

struct uzlib_uncomp {
  const uint8_t* source = nullptr;
  const uint8_t* source_limit = nullptr;
};

class InflateReader {
 public:
  static constexpr size_t RING_BYTES = 32768;

  bool init(bool) { return InflateReaderStub::initSucceeds; }
  bool initWithRing(uint8_t*) { return InflateReaderStub::initSucceeds; }
  void setSource(const uint8_t*, size_t) {}
  void setReadCallback(int (*)(uzlib_uncomp*)) { InflateReaderStub::callbackSet = true; }
  bool read(uint8_t* destination, const size_t length) {
    if (!InflateReaderStub::readSucceeds) return false;
    std::fill_n(destination, length, uint8_t{0x41});
    return true;
  }
};
