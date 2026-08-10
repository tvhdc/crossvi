#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
bool panicReportPersisted();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
}  // namespace HalSystem
