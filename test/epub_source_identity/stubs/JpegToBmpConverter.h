#pragma once

#include <HalStorage.h>

#include "ThumbnailConverterStub.h"

class JpegToBmpConverter {
 public:
  static constexpr size_t MAX_ONE_BIT_OUTPUTS = 2;
  struct OneBitBmpTarget {
    Print* output = nullptr;
    int targetMaxWidth = 0;
    int targetMaxHeight = 0;
    bool crop = true;
  };

  static bool jpegFileToBmpStream(HalFile&, Print&, bool = true) { return false; }
  static bool jpegFileToBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile&, Print& out, const int width, const int height,
                                              const bool crop = true) {
    return ThumbnailConverterStub::convert(out, width, height, crop);
  }
  static bool jpegFileRangeToBmpStream(HalFile&, uint64_t, uint32_t, Print&, bool = true) { return false; }
  static bool jpegFileRangeTo1BitBmpStreamWithSize(HalFile&, const uint64_t offset, const uint32_t length, Print& out,
                                                   const int width, const int height, const bool crop = true) {
    ++ThumbnailConverterStub::rangedCallCount;
    ThumbnailConverterStub::lastSourceOffset = offset;
    ThumbnailConverterStub::lastSourceLength = length;
    return ThumbnailConverterStub::convert(out, width, height, crop);
  }
  static uint8_t jpegFileTo1BitBmpStreamsWithSize(HalFile&, const OneBitBmpTarget* targets, const size_t targetCount) {
    return convertBatch(targets, targetCount);
  }
  static uint8_t jpegFileRangeTo1BitBmpStreamsWithSize(HalFile&, const uint64_t offset, const uint32_t length,
                                                       const OneBitBmpTarget* targets, const size_t targetCount) {
    ++ThumbnailConverterStub::rangedCallCount;
    ThumbnailConverterStub::lastSourceOffset = offset;
    ThumbnailConverterStub::lastSourceLength = length;
    return convertBatch(targets, targetCount);
  }

 private:
  static uint8_t convertBatch(const OneBitBmpTarget* targets, const size_t targetCount) {
    if (!targets || targetCount == 0 || targetCount > MAX_ONE_BIT_OUTPUTS) return 0;
    ++ThumbnailConverterStub::batchCallCount;
    uint8_t result = 0;
    for (size_t index = 0; index < targetCount; ++index) {
      if (targets[index].output &&
          ThumbnailConverterStub::convert(*targets[index].output, targets[index].targetMaxWidth,
                                          targets[index].targetMaxHeight, targets[index].crop)) {
        result |= static_cast<uint8_t>(1U << index);
      }
    }
    return result;
  }
};
