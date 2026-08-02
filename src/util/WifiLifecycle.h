#pragma once

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

namespace WifiLifecycle {

// Release the ESP32 Wi-Fi driver and its netifs without rebooting the device.
// Callers may keep their existing restart path as a fallback when teardown
// fails; this avoids trading a visible reboot for a half-alive radio stack.
inline bool shutDown() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return true;

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(20);
  const bool inactive = WiFi.getMode() == WIFI_MODE_NULL;
  if (!inactive) LOG_ERR("WIFI", "Wi-Fi teardown failed; restart fallback required");
  return inactive;
}

}  // namespace WifiLifecycle
