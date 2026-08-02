#pragma once

#include <esp_err.h>
#include <esp_task_wdt.h>

// Network handlers can run in an activity task that is not subscribed to the
// task watchdog on every supported ESP-IDF/Arduino combination.  Resetting an
// unsubscribed task returns an error (and is noisy on some IDF versions), so
// keep the long-I/O heartbeat conditional and allocation-free.
inline void resetTaskWatchdogIfSubscribed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
  }
}
