#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct SleepImagePlacement {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  float scale = 1.0f;
};

constexpr uint8_t SLEEP_IMAGE_MIN_ZOOM = 50;
constexpr uint8_t SLEEP_IMAGE_MAX_ZOOM = 200;
constexpr uint8_t SLEEP_IMAGE_DEFAULT_ZOOM = 100;
constexpr int SLEEP_IMAGE_FRONT_ZOOM_STEP = 1;
constexpr int SLEEP_IMAGE_SIDE_ZOOM_STEP = 5;
constexpr int SLEEP_IMAGE_MOVE_STEP = 8;
constexpr int SLEEP_IMAGE_FAST_MOVE_STEP = 32;
constexpr unsigned long SLEEP_IMAGE_FAST_MOVE_HOLD_MS = 1200;
constexpr unsigned long SLEEP_IMAGE_RESET_HOLD_MS = 700;

constexpr int sleepImageMoveStep(const unsigned long heldMs) {
  return heldMs >= SLEEP_IMAGE_FAST_MOVE_HOLD_MS ? SLEEP_IMAGE_FAST_MOVE_STEP : SLEEP_IMAGE_MOVE_STEP;
}

constexpr bool shouldResetSleepImageTransform(const unsigned long heldMs) {
  return heldMs >= SLEEP_IMAGE_RESET_HOLD_MS;
}

inline SleepImagePlacement calculateSleepImagePlacement(const int screenWidth, const int screenHeight,
                                                        const int sourceWidth, const int sourceHeight,
                                                        const uint8_t zoomPercent, const int offsetX,
                                                        const int offsetY) {
  SleepImagePlacement out;
  if (screenWidth <= 0 || screenHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return out;

  const float scaleX = static_cast<float>(screenWidth) / sourceWidth;
  const float scaleY = static_cast<float>(screenHeight) / sourceHeight;
  float scale = std::min({scaleX, scaleY, 1.0f});
  scale *= static_cast<float>(std::clamp<uint8_t>(zoomPercent, SLEEP_IMAGE_MIN_ZOOM, SLEEP_IMAGE_MAX_ZOOM)) / 100.0f;

  out.width = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
  out.height = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));
  out.x = (screenWidth - out.width) / 2 + std::clamp(offsetX, -screenWidth / 2, screenWidth / 2);
  out.y = (screenHeight - out.height) / 2 + std::clamp(offsetY, -screenHeight / 2, screenHeight / 2);
  out.scale = scale;
  return out;
}
