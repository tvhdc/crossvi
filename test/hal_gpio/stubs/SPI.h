#pragma once

class SpiStub {
 public:
  void begin(int, int, int, int) {}
};

inline SpiStub SPI;
