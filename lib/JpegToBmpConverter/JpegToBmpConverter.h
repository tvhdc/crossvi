#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, uint64_t sourceOffset, uint32_t sourceLength,
                                          Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                          bool crop = true);

 public:
  static constexpr size_t MAX_ONE_BIT_OUTPUTS = 2;

  struct OneBitBmpTarget {
    Print* output = nullptr;
    int targetMaxWidth = 0;
    int targetMaxHeight = 0;
    bool crop = true;
  };

  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool crop = true);
  static bool jpegFileRangeToBmpStream(HalFile& jpegFile, uint64_t sourceOffset, uint32_t sourceLength, Print& bmpOut,
                                       bool crop = true);
  static bool jpegFileRangeTo1BitBmpStreamWithSize(HalFile& jpegFile, uint64_t sourceOffset, uint32_t sourceLength,
                                                   Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                                   bool crop = true);
  // Decode once and fan the grayscale rows out to at most two independent 1-bit BMP streams.
  // Bit N in the return value is set only when target N completed successfully.
  static uint8_t jpegFileTo1BitBmpStreamsWithSize(HalFile& jpegFile, const OneBitBmpTarget* targets,
                                                  size_t targetCount);
  static uint8_t jpegFileRangeTo1BitBmpStreamsWithSize(HalFile& jpegFile, uint64_t sourceOffset, uint32_t sourceLength,
                                                       const OneBitBmpTarget* targets, size_t targetCount);
};
