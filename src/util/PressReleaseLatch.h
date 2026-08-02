#pragma once

class PressReleaseLatch {
  bool armed_ = false;

 public:
  bool update(const bool pressed, const bool released) {
    if (pressed) armed_ = true;
    if (!released) return false;
    const bool ownedRelease = armed_;
    armed_ = false;
    return ownedRelease;
  }

  [[nodiscard]] bool isArmed() const { return armed_; }
  void reset() { armed_ = false; }
};
