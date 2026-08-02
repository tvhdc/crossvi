#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class TimeSettingsActivity final : public Activity {
 public:
  static constexpr int ITEM_COUNT = 3;

  explicit TimeSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TimeSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;

  void handleSelection();
};
