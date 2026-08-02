#pragma once

#include <cstdint>

namespace TiltPageTurnPolicy {

// A 0.50 g lateral gravity component corresponds to a deliberate tilt of
// about 30 degrees.  Two consecutive 20 Hz samples reject isolated sensor
// spikes, while the lower neutral threshold supplies hysteresis.
constexpr float TRIGGER_THRESHOLD_G = 0.50f;
constexpr float NEUTRAL_THRESHOLD_G = 0.17f;
constexpr uint8_t REQUIRED_SAMPLES = 2;
constexpr uint32_t COOLDOWN_MS = 600;

enum class Direction : int8_t { None = 0, Forward = 1, Back = -1 };

struct GestureState {
  bool armed = false;
  bool inTilt = false;
  bool hasTriggered = false;
  Direction candidate = Direction::None;
  uint8_t candidateSamples = 0;
  uint8_t neutralSamples = 0;
  uint32_t lastTriggerMs = 0;
  uint32_t triggerRenderGeneration = 0;
  bool awaitingRender = false;

  void requireNeutral() {
    armed = false;
    inTilt = false;
    candidate = Direction::None;
    candidateSamples = 0;
    neutralSamples = 0;
  }
};

constexpr int16_t decodeBigEndian(const uint8_t high, const uint8_t low) {
  return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low);
}

constexpr float selectedAxis(const float x, const float y, const uint8_t orientation, const uint8_t mode) {
  const bool leftToNext = mode == 2;
  switch (orientation) {
    case 0:  // Portrait
      return leftToNext ? -x : x;
    case 2:  // Portrait inverted
      return leftToNext ? x : -x;
    case 1:  // Landscape clockwise
      return leftToNext ? y : -y;
    case 3:  // Landscape counter-clockwise
      return leftToNext ? -y : y;
    default:
      return x;
  }
}

constexpr bool shouldTurnNext(const float selectedAxis, const float threshold) { return selectedAxis > threshold; }

inline Direction updateGesture(GestureState& state, const float selectedAxis, const uint32_t now, const bool pageReady,
                               const uint32_t completedRenderGeneration = 0) {
  // A page-turn gesture is not complete until a later render generation has
  // finished. This closes the window between requestUpdate() and the render
  // task acquiring RenderLock, which a mutex-only gate cannot observe.
  if (state.awaitingRender) {
    if (completedRenderGeneration == state.triggerRenderGeneration) return Direction::None;
    state.awaitingRender = false;
    state.requireNeutral();
    return Direction::None;
  }

  if (!pageReady) {
    state.requireNeutral();
    return Direction::None;
  }

  const float magnitude = selectedAxis < 0.0f ? -selectedAxis : selectedAxis;
  const bool neutral = magnitude < NEUTRAL_THRESHOLD_G;

  // Waking the sensor or finishing a page refresh never arms a device that is
  // already tilted.  It must be held level for two samples first.
  if (!state.armed) {
    state.candidate = Direction::None;
    state.candidateSamples = 0;
    if (neutral) {
      if (++state.neutralSamples >= REQUIRED_SAMPLES) {
        state.armed = true;
        state.neutralSamples = 0;
      }
    } else {
      state.neutralSamples = 0;
    }
    return Direction::None;
  }

  if (state.inTilt) {
    if (neutral) {
      if (++state.neutralSamples >= REQUIRED_SAMPLES) {
        state.inTilt = false;
        state.neutralSamples = 0;
      }
    } else {
      state.neutralSamples = 0;
    }
    return Direction::None;
  }

  if (magnitude <= TRIGGER_THRESHOLD_G) {
    state.candidate = Direction::None;
    state.candidateSamples = 0;
    return Direction::None;
  }

  const Direction direction = selectedAxis > 0.0f ? Direction::Forward : Direction::Back;
  if (state.candidate == direction) {
    ++state.candidateSamples;
  } else {
    state.candidate = direction;
    state.candidateSamples = 1;
  }
  if (state.candidateSamples < REQUIRED_SAMPLES) return Direction::None;

  state.candidate = Direction::None;
  state.candidateSamples = 0;
  state.inTilt = true;
  if (state.hasTriggered && static_cast<uint32_t>(now - state.lastTriggerMs) < COOLDOWN_MS) {
    return Direction::None;
  }
  state.hasTriggered = true;
  state.lastTriggerMs = now;
  state.triggerRenderGeneration = completedRenderGeneration;
  state.awaitingRender = true;
  return direction;
}

}  // namespace TiltPageTurnPolicy
