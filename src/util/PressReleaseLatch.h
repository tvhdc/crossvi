#pragma once

#include <cstdint>

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

class ReleaseDebounceGuard {
  uint32_t lastAcceptedAt_ = 0;
  bool accepted_ = false;

 public:
  bool accept(const uint32_t now, const uint32_t minimumGapMs) {
    if (accepted_ && static_cast<uint32_t>(now - lastAcceptedAt_) < minimumGapMs) return false;
    accepted_ = true;
    lastAcceptedAt_ = now;
    return true;
  }

  void reset() {
    accepted_ = false;
    lastAcceptedAt_ = 0;
  }
};
