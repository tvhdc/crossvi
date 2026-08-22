#include <HalStorage.h>

#include "SleepImageValidation.h"

namespace SleepImageValidation {
namespace {
bool startsWith(const char* path, const uint8_t marker) {
  HalFile file;
  uint8_t value = 0;
  return path && Storage.openFileForRead("SLP", path, file) && file.read(&value, 1) == 1 && file.close() &&
         value == marker;
}
}  // namespace

bool normalBmp(const char* path) { return startsWith(path, 'N'); }
bool overlayBmp(const char* path) { return startsWith(path, 'B'); }
bool overlayPng(const char* path) { return startsWith(path, 'P'); }
}  // namespace SleepImageValidation
