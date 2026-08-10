#include "ButtonNavigator.h"

#include <Arduino.h>

#include <algorithm>

const MappedInputManager* ButtonNavigator::mappedInput = nullptr;

void ButtonNavigator::onNext(const Callback& callback) {
  onNextPress(callback);
  onNextContinuous(callback);
}

void ButtonNavigator::onPrevious(const Callback& callback) {
  onPreviousPress(callback);
  onPreviousContinuous(callback);
}

void ButtonNavigator::onPressAndContinuous(const Buttons& buttons, const Callback& callback) {
  onPress(buttons, callback);
  onContinuous(buttons, callback);
}

void ButtonNavigator::onNextPress(const Callback& callback) { onPress(getNextButtons(), callback); }

void ButtonNavigator::onPreviousPress(const Callback& callback) { onPress(getPreviousButtons(), callback); }

void ButtonNavigator::onNextRelease(const Callback& callback) { onRelease(getNextButtons(), callback); }

void ButtonNavigator::onPreviousRelease(const Callback& callback) { onRelease(getPreviousButtons(), callback); }

void ButtonNavigator::onNextContinuous(const Callback& callback) { onContinuous(getNextButtons(), callback); }

void ButtonNavigator::onPreviousContinuous(const Callback& callback) { onContinuous(getPreviousButtons(), callback); }

void ButtonNavigator::onPress(const Buttons& buttons, const Callback& callback) {
  const bool wasPressed = std::any_of(buttons.begin(), buttons.end(), [this](const MappedInputManager::Button button) {
    if (mappedInput == nullptr) return false;
    const bool pressed = mappedInput->wasPressed(button);
    latch(button).update(pressed, false);
    return pressed;
  });

  if (wasPressed && acceptNavigationEdge()) {
    callback();
  }
}

void ButtonNavigator::onRelease(const Buttons& buttons, const Callback& callback) {
  const bool wasReleased = std::any_of(buttons.begin(), buttons.end(), [this](const MappedInputManager::Button button) {
    return mappedInput != nullptr &&
           latch(button).update(mappedInput->wasPressed(button), mappedInput->wasReleased(button));
  });

  if (wasReleased) {
    if (lastContinuousNavTime == 0 && acceptNavigationEdge()) {
      callback();
    }

    lastContinuousNavTime = 0;
  }
}

void ButtonNavigator::onContinuous(const Buttons& buttons, const Callback& callback) {
  const bool isPressed = std::any_of(buttons.begin(), buttons.end(), [this](const MappedInputManager::Button button) {
    if (mappedInput == nullptr) return false;
    latch(button).update(mappedInput->wasPressed(button), false);
    return latch(button).isArmed() && mappedInput->isPressed(button) && shouldNavigateContinuously(button);
  });

  if (isPressed && acceptNavigationEdge()) {
    callback();
    lastContinuousNavTime = millis();
  }
}

bool ButtonNavigator::shouldNavigateContinuously(const MappedInputManager::Button button) const {
  if (!mappedInput) return false;

  const bool buttonHeldLongEnough = mappedInput->getHeldTime(button) >= continuousStartMs;
  const bool navigationIntervalElapsed = (millis() - lastContinuousNavTime) >= continuousIntervalMs;

  return buttonHeldLongEnough && navigationIntervalElapsed;
}

bool ButtonNavigator::acceptNavigationEdge() {
  // X3/X4 buttons use ADC resistor ladders. A noisy physical transition can
  // look like several complete press/release cycles even after the low-level
  // debounce window. Treat that short burst as one navigation action while
  // leaving deliberate clicks and the 500 ms hold-repeat cadence unchanged.
  return navigationEdgeGuard_.accept(static_cast<uint32_t>(millis()), NAVIGATION_EDGE_GUARD_MS);
}

int ButtonNavigator::nextIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the next index with wrap-around
  return (currentIndex + 1) % totalItems;
}

int ButtonNavigator::previousIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the previous index with wrap-around
  return (currentIndex + totalItems - 1) % totalItems;
}

int ButtonNavigator::nextPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return nextIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex < lastPageIndex) {
    return (currentPageIndex + 1) * itemsPerPage;
  }

  return 0;
}

int ButtonNavigator::previousPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return previousIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex > 0) {
    return (currentPageIndex - 1) * itemsPerPage;
  }

  return lastPageIndex * itemsPerPage;
}
