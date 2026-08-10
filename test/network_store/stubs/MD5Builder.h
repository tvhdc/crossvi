#pragma once

#include <WString.h>

class MD5Builder {
 public:
  void begin() {}
  void add(const char*) {}
  void calculate() {}
  String toString() const { return String("00000000000000000000000000000000"); }
};
