#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class HalGPIO {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  std::array<bool, 7> pressed{};
  std::array<bool, 7> released{};
  std::array<bool, 7> held{};
  std::array<unsigned long, 7> heldTime{};

  void update() {}
  bool isPressed(const uint8_t button) const { return held[button]; }
  bool wasPressed(const uint8_t button) const { return pressed[button]; }
  bool wasReleased(const uint8_t button) const { return released[button]; }
  bool wasAnyPressed() const { return any(pressed); }
  bool wasAnyReleased() const { return any(released); }
  unsigned long getHeldTime() const { return 0; }
  unsigned long getHeldTime(const uint8_t button) const { return heldTime[button]; }

 private:
  static bool any(const std::array<bool, 7>& values) {
    for (const bool value : values) {
      if (value) return true;
    }
    return false;
  }
};
