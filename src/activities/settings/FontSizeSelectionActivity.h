#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FontSizeSelectionActivity final : public Activity {
 public:
  explicit FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     bool persistInvalidSelection = true);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void previewSelection(int index);
  std::string sizeLabel(int index) const;
  std::string actualSizeLabel(int index) const;

  bool persistInvalidSelection_ = true;
  ButtonNavigator buttonNavigator_;
  ThemeMetrics metrics_ = {};
  uint8_t originalSize_ = 0;
  int selectedIndex_ = 0;
};
