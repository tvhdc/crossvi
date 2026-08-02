#pragma once

#include <cstdint>
#include <string>

class CrossPointState {
 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;
  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentSleepPos = 0;
  uint8_t recentSleepFill = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;

  void pushRecentSleep(const uint16_t value) {
    recentSleepImages[recentSleepPos] = value;
    recentSleepPos = static_cast<uint8_t>((recentSleepPos + 1) % SLEEP_RECENT_COUNT);
    if (recentSleepFill < SLEEP_RECENT_COUNT) ++recentSleepFill;
  }
};
