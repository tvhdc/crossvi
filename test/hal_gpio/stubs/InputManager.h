#pragma once

#include <Arduino.h>

#include <cstdint>

namespace BoardConfig {
enum class Board { XteinkX4, XteinkX3, XteinkX3Uc8279 };
inline void selectDevice(Board) {}
}  // namespace BoardConfig

class InputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
  static constexpr int POWER_BUTTON_PIN = 3;

  static inline uint8_t rawState = 0;
  static inline uint8_t currentState = 0;
  static inline uint8_t pressedEvents = 0;
  static inline uint8_t releasedEvents = 0;
  static inline uint32_t pressStart = 0;
  static inline uint32_t pressFinish = 0;
  static inline uint32_t powerPressStart = 0;
  static inline uint32_t powerPressFinish = 0;

  static void reset() {
    rawState = 0;
    currentState = 0;
    pressedEvents = 0;
    releasedEvents = 0;
    pressStart = 0;
    pressFinish = 0;
    powerPressStart = 0;
    powerPressFinish = 0;
  }

  void begin() {}
  uint8_t getState() { return rawState; }

  void update() {
    const uint8_t previous = currentState;
    pressedEvents = rawState & ~previous;
    releasedEvents = previous & ~rawState;
    if (pressedEvents != 0 && previous == 0) {
      pressStart = ArduinoFake::now;
    }
    if (releasedEvents != 0 && rawState == 0) {
      pressFinish = ArduinoFake::now;
    }
    if ((pressedEvents & (1U << BTN_POWER)) != 0) {
      powerPressStart = ArduinoFake::now;
    }
    if ((releasedEvents & (1U << BTN_POWER)) != 0) {
      powerPressFinish = ArduinoFake::now;
    }
    currentState = rawState;
  }

  bool isPressed(uint8_t button) const { return (currentState & (1U << button)) != 0; }
  bool wasPressed(uint8_t button) const { return (pressedEvents & (1U << button)) != 0; }
  bool wasAnyPressed() const { return pressedEvents != 0; }
  bool wasReleased(uint8_t button) const { return (releasedEvents & (1U << button)) != 0; }
  bool wasAnyReleased() const { return releasedEvents != 0; }
  bool isDebouncePending() const { return rawState != currentState; }

  unsigned long getHeldTime() const {
    const uint32_t elapsed = currentState != 0 ? ArduinoFake::now - pressStart : pressFinish - pressStart;
    return elapsed;
  }

  unsigned long getPowerButtonHeldTime() const {
    const uint32_t elapsed =
        isPressed(BTN_POWER) ? ArduinoFake::now - powerPressStart : powerPressFinish - powerPressStart;
    return elapsed;
  }
};
