#pragma once
#include "activities/Activity.h"

class BootActivity final : public Activity {
  const bool minimalWakeScreen_;

 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool minimalWakeScreen = false)
      : Activity("Boot", renderer, mappedInput), minimalWakeScreen_(minimalWakeScreen) {}
  void onEnter() override;
};
