#pragma once

#include <cstdint>

class GfxRenderer;

struct DirectPixelWriter {
  void init(GfxRenderer&) {}
  void beginRow(int) {}
  void bandColRange(int, int width, int& start, int& end) const {
    start = 0;
    end = width;
  }
  void writePixel(int, uint8_t) const {}
};
