#include "MemoryBudget.h"

#include <Arduino.h>
#include <Logging.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace MemoryBudget {

Snapshot snapshot() {
  Snapshot result;
  result.freeHeap = ESP.getFreeHeap();
  result.maxAllocHeap = ESP.getMaxAllocHeap();
  result.minFreeHeap = ESP.getMinFreeHeap();
#if defined(ARDUINO_ARCH_ESP32)
  result.stackWatermark = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
#endif
  return result;
}

void logStage(const char* origin, const char* stage) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const Snapshot memory = snapshot();
  LOG_DBG(origin, "%s free=%u maxalloc=%u minfree=%u stack=%u", stage, memory.freeHeap, memory.maxAllocHeap,
          memory.minFreeHeap, memory.stackWatermark);
#else
  (void)origin;
  (void)stage;
#endif
}

}  // namespace MemoryBudget
