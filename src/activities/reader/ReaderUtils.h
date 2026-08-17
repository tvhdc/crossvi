#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long CONFIRM_HOLD_MS = 500;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;
constexpr uint8_t DEFAULT_AUTO_PAGE_TURN_SECONDS = 30;
constexpr int8_t MAX_QUEUED_PAGE_TURNS = 8;
constexpr size_t GRAYSCALE_STRIP_SCRATCH_BYTES = 13U * 1024U;

inline void queuePageTurns(int8_t& pending, const int delta) {
  pending = static_cast<int8_t>(std::clamp(static_cast<int>(pending) + delta, -static_cast<int>(MAX_QUEUED_PAGE_TURNS),
                                           static_cast<int>(MAX_QUEUED_PAGE_TURNS)));
}

inline bool takeQueuedPageTurn(int8_t& pending, bool& forward) {
  if (pending == 0) return false;
  forward = pending > 0;
  pending += forward ? -1 : 1;
  return true;
}

inline int grayscaleStripRows(const int widthBytes, const int height) {
  if (widthBytes <= 0 || height <= 0) return 0;
  const size_t rows = std::max<size_t>(1, GRAYSCALE_STRIP_SCRATCH_BYTES / static_cast<size_t>(widthBytes));
  return std::min<int>(height, static_cast<int>(rows));
}

enum class HoldRelease : uint8_t { None, Short, Long };

struct HoldGestureState {
  bool pressed = false;
  bool longHandled = false;

  void onPress() {
    pressed = true;
    longHandled = false;
  }

  bool onHold(const unsigned long heldMs, const unsigned long thresholdMs) {
    if (!pressed || longHandled || heldMs < thresholdMs) return false;
    longHandled = true;
    return true;
  }

  HoldRelease onRelease() {
    if (!pressed) return HoldRelease::None;
    const HoldRelease result = longHandled ? HoldRelease::Long : HoldRelease::Short;
    reset();
    return result;
  }

  void reset() {
    pressed = false;
    longHandled = false;
  }
};

struct PageTurnGestureState {
  HoldGestureState previous;
  HoldGestureState next;

  void reset() {
    previous.reset();
    next.reset();
  }
};

inline uint8_t autoPageTurnShortcutSeconds(const uint8_t previousSeconds) {
  return previousSeconds == 0 ? DEFAULT_AUTO_PAGE_TURN_SECONDS : previousSeconds;
}

inline bool consumeInitialRelease(bool& armed, const bool wasReleased, const bool isPressed) {
  if (!armed) return false;
  if (wasReleased || !isPressed) armed = false;
  return true;
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnGestureResult {
  bool prev;
  bool next;
  bool longPress;
};

inline PageTurnGestureResult detectPageTurnGesture(const MappedInputManager& input, PageTurnGestureState& state) {
  const bool tiltEnabled = SETTINGS.tiltPageTurn != CrossPointSettings::TILT_OFF;
  const bool tiltNext = tiltEnabled && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = tiltEnabled && halTiltSensor.wasTiltedBack();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);

  if (SETTINGS.longPressButtonBehavior == SETTINGS.OFF) {
    state.reset();
    return {input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton) || tiltPrev,
            input.wasPressed(MappedInputManager::Button::PageForward) || input.wasPressed(nextButton) || tiltNext ||
                powerTurn,
            false};
  }

  const bool previousPressed = input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton);
  const bool nextPressed = input.wasPressed(MappedInputManager::Button::PageForward) || input.wasPressed(nextButton);
  const bool previousHeld = input.isPressed(MappedInputManager::Button::PageBack) || input.isPressed(prevButton);
  const bool nextHeld = input.isPressed(MappedInputManager::Button::PageForward) || input.isPressed(nextButton);
  const bool previousReleased =
      input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton);
  const bool nextReleased = input.wasReleased(MappedInputManager::Button::PageForward) || input.wasReleased(nextButton);

  if (previousPressed) state.previous.onPress();
  if (nextPressed) state.next.onPress();

  const auto heldPreviousButton = input.isPressed(prevButton) ? prevButton : MappedInputManager::Button::PageBack;
  const auto heldNextButton = input.isPressed(nextButton) ? nextButton : MappedInputManager::Button::PageForward;
  if (previousHeld && state.previous.onHold(input.getHeldTime(heldPreviousButton), SKIP_HOLD_MS)) {
    return {true, false, true};
  }
  if (nextHeld && state.next.onHold(input.getHeldTime(heldNextButton), SKIP_HOLD_MS)) {
    return {false, true, true};
  }

  const HoldRelease previousRelease = previousReleased ? state.previous.onRelease() : HoldRelease::None;
  const HoldRelease nextRelease = nextReleased ? state.next.onRelease() : HoldRelease::None;
  return {tiltPrev || previousRelease == HoldRelease::Short, tiltNext || powerTurn || nextRelease == HoldRelease::Short,
          false};
}

inline bool isLongPageTurnRelease(const MappedInputManager& input, const PageTurnGestureState& state) {
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  return (state.previous.longHandled &&
          (input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton))) ||
         (state.next.longHandled &&
          (input.wasReleased(MappedInputManager::Button::PageForward) || input.wasReleased(nextButton)));
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, std::unique_ptr<uint8_t[]>& scratch, size_t& scratchCapacity,
                       RenderFn&& renderFn) {
  if (!renderer.supportsStripGrayscale()) return;

  const int height = renderer.getDisplayHeight();
  const int widthBytes = renderer.getDisplayWidthBytes();
  const int stripRows = grayscaleStripRows(widthBytes, height);
  if (stripRows <= 0) return;
  const size_t requiredScratch = static_cast<size_t>(widthBytes) * stripRows;
  if (scratchCapacity < requiredScratch) {
    scratch.reset();
    scratchCapacity = 0;
    scratch.reset(new (std::nothrow) uint8_t[requiredScratch]);
    if (scratch) scratchCapacity = requiredScratch;
  }
  if (!scratch) {
    LOG_ERR("READER", "OOM: grayscale strip scratch (%u bytes)", static_cast<unsigned>(requiredScratch));
    return;
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  for (int y = 0; y < height; y += stripRows) {
    const int rows = std::min(stripRows, height - y);
    renderer.beginStripTarget(scratch.get(), y, rows);
    renderer.clearScreen(0x00);
    renderFn();
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  for (int y = 0; y < height; y += stripRows) {
    const int rows = std::min(stripRows, height - y);
    renderer.beginStripTarget(scratch.get(), y, rows);
    renderer.clearScreen(0x00);
    renderFn();
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
  }

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  std::unique_ptr<uint8_t[]> scratch;
  size_t scratchCapacity = 0;
  renderAntiAliased(renderer, scratch, scratchCapacity, std::forward<RenderFn>(renderFn));
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
// Long press (>= GO_BACK_OR_HOME_MS) opens the file browser. A short press
// returns Home. Keeping this fixed avoids a hidden navigation mode that could
// make Back leave the reader for an unexpected screen.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome,
                                 BackNavCallback beforeNavigate = {nullptr, nullptr}) {
  const auto prepareNavigation = [beforeNavigate]() {
    if (beforeNavigate.fn) beforeNavigate.fn(beforeNavigate.ctx);
  };
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Back) >= GO_BACK_OR_HOME_MS) {
    prepareNavigation();
    activityManager.goToFileBrowser(filePath);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Back) < GO_BACK_OR_HOME_MS) {
    prepareNavigation();
    goHome.fn(goHome.ctx);
    return true;
  }
  return false;
}

}  // namespace ReaderUtils
