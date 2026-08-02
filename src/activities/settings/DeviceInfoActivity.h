#pragma once

#include "activities/Activity.h"

class DeviceInfoActivity final : public Activity {
 public:
  explicit DeviceInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeviceInfo", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint64_t sdTotalBytes = 0;
  bool storageAvailable = false;
};
