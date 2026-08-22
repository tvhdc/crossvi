#pragma once

#include <cstdint>
#include <string>

class HalFile;

namespace SleepImageValidation {

struct Bmp32Header {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint64_t pixelOffset = 0;
  uint32_t rowBytes = 0;
};

enum class Bmp32HeaderStatus : uint8_t { Invalid, Not32Bit, Valid };

Bmp32HeaderStatus readBmp32Header(HalFile& file, Bmp32Header& out);
bool normalBmp(const char* path);
bool overlayBmp(const char* path);
bool overlayPng(const char* path);

inline bool normalBmp(const std::string& path) { return normalBmp(path.c_str()); }
inline bool overlayBmp(const std::string& path) { return overlayBmp(path.c_str()); }
inline bool overlayPng(const std::string& path) { return overlayPng(path.c_str()); }

}  // namespace SleepImageValidation
