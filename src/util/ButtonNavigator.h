#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "MappedInputManager.h"
#include "util/PressReleaseLatch.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::array<MappedInputManager::Button, 1>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static constexpr uint32_t NAVIGATION_EDGE_GUARD_MS = 150;
  static constexpr size_t BUTTON_COUNT = static_cast<size_t>(MappedInputManager::Button::NavPrevious) + 1;
  std::array<PressReleaseLatch, BUTTON_COUNT> pressLatches{};
  ReleaseDebounceGuard navigationEdgeGuard_;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously(MappedInputManager::Button button) const;
  [[nodiscard]] bool acceptNavigationEdge();
  PressReleaseLatch& latch(MappedInputManager::Button button) { return pressLatches[static_cast<size_t>(button)]; }

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // Navigation uses the logical NavNext / NavPrevious buttons; MappedInputManager::mapButton resolves
  // them to physical buttons and applies any orientation-based direction swap, so this stays settings-free.
  [[nodiscard]] static constexpr Buttons getNextButtons() { return {MappedInputManager::Button::NavNext}; }
  [[nodiscard]] static constexpr Buttons getPreviousButtons() { return {MappedInputManager::Button::NavPrevious}; }
};
