#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FontSizeSelectionActivity final : public Activity {
 public:
  explicit FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void previewSelection(int index);
  void buildSizeOptions();
  std::string sizeLabel(int index) const;

  ButtonNavigator buttonNavigator_;
  ThemeMetrics metrics_ = {};
  uint8_t originalFontFamily_ = 0;
  uint8_t originalSize_ = 0;
  char originalSdFontFamilyName_[32] = {};
  int selectedIndex_ = 0;
  std::vector<uint8_t> sizeOptions_;
};
