#pragma once

#include <HalStorage.h>

#include "ThumbnailConverterStub.h"

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile&, Print&, bool = true) { return false; }
  static bool pngFileToBmpStreamWithSize(HalFile&, Print&, int, int, bool = true) { return false; }
  static bool pngFileTo1BitBmpStreamWithSize(HalFile&, Print& out, const int width, const int height,
                                             const bool crop = true) {
    return ThumbnailConverterStub::convert(out, width, height, crop);
  }
};
