#pragma once

enum esp_sleep_wakeup_cause_t { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_GPIO };
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_DEEPSLEEP };

namespace EspFake {
inline esp_sleep_wakeup_cause_t wakeupCause = ESP_SLEEP_WAKEUP_UNDEFINED;
inline esp_reset_reason_t resetReason = ESP_RST_POWERON;
}  // namespace EspFake

inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return EspFake::wakeupCause; }
inline esp_reset_reason_t esp_reset_reason() { return EspFake::resetReason; }
