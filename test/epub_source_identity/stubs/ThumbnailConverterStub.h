#pragma once

#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ThumbnailConverterStub {

struct Call {
  int width = 0;
  int height = 0;
  bool crop = false;
};

inline bool enabled = false;
inline size_t failCall = 0;
inline size_t callCount = 0;
inline size_t batchCallCount = 0;
inline size_t rangedCallCount = 0;
inline uint64_t lastSourceOffset = 0;
inline uint32_t lastSourceLength = 0;
inline std::array<Call, 4> calls{};
inline void (*afterCall)(size_t) = nullptr;

inline void reset(const bool enable = false) {
  enabled = enable;
  failCall = 0;
  callCount = 0;
  batchCallCount = 0;
  rangedCallCount = 0;
  lastSourceOffset = 0;
  lastSourceLength = 0;
  calls = {};
  afterCall = nullptr;
}

inline void write16(Print& out, const uint16_t value) {
  out.write(static_cast<uint8_t>(value));
  out.write(static_cast<uint8_t>(value >> 8U));
}

inline void write32(Print& out, const uint32_t value) {
  out.write(static_cast<uint8_t>(value));
  out.write(static_cast<uint8_t>(value >> 8U));
  out.write(static_cast<uint8_t>(value >> 16U));
  out.write(static_cast<uint8_t>(value >> 24U));
}

inline bool convert(Print& out, const int width, const int height, const bool crop) {
  const size_t call = ++callCount;
  if (call <= calls.size()) calls[call - 1] = {width, height, crop};
  if (!enabled || (failCall != 0 && call == failCall) || width <= 0 || height <= 0) return false;

  const uint32_t rowBytes = (static_cast<uint32_t>(width) + 31U) / 32U * 4U;
  const uint32_t imageSize = rowBytes * static_cast<uint32_t>(height);
  write16(out, 0x4D42U);
  write32(out, 62U + imageSize);
  write32(out, 0U);
  write32(out, 62U);
  write32(out, 40U);
  write32(out, static_cast<uint32_t>(width));
  write32(out, static_cast<uint32_t>(height));
  write16(out, 1U);
  write16(out, 1U);
  write32(out, 0U);
  write32(out, imageSize);
  write32(out, 0U);
  write32(out, 0U);
  write32(out, 2U);
  write32(out, 0U);
  const uint8_t palette[8] = {0, 0, 0, 0, 255, 255, 255, 0};
  if (out.write(palette, sizeof(palette)) != sizeof(palette)) return false;
  std::array<uint8_t, 64> row{};
  row.fill(0xAAU);
  for (int y = 0; y < height; ++y) {
    size_t remaining = rowBytes;
    while (remaining > 0) {
      const size_t chunk = remaining < row.size() ? remaining : row.size();
      if (out.write(row.data(), chunk) != chunk) return false;
      remaining -= chunk;
    }
  }
  if (afterCall) afterCall(call);
  return true;
}

}  // namespace ThumbnailConverterStub
