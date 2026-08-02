#pragma once

#include <cstdint>

class HalDisplay {
 public:
  uint16_t getDisplayWidth() const { return 528; }
  uint16_t getDisplayHeight() const { return 792; }
};

inline HalDisplay display;
