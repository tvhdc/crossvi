#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FontSelectionActivity final : public Activity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry, bool persistInvalidSelection = true);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void selectIndex(int index);
  void handleSelection();
  void applyFontSelection(int index, bool preparePreview = false);
  void renderPreviewPane(int top, int height, int fontId, const char* fontName, bool cachedCustomPreview = false);

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  const SdCardFontRegistry* registry_;
  bool persistInvalidSelection_ = true;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
  int previewFontIndex_ = 0;
  int preparedPreviewFontId_ = 0;
  int customPreviewAttemptedIndex_ = -1;
  int customPreviewSnapshotIndex_ = -1;
  bool customPreviewPending_ = false;
  std::unique_ptr<uint8_t[]> customPreviewSnapshot_;
  size_t customPreviewSnapshotSize_ = 0;
  uint8_t originalFontFamily_ = 0;
  uint8_t originalFontSize_ = 0;
  uint8_t preferredPointSize_ = 14;
  char originalSdFontFamilyName_[32] = {};

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
