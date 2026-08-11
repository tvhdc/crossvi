#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SettingsActivity.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class TextBlock;

class TextSettingsActivity final : public Activity {
 public:
  enum Tab : uint8_t { Font = 0, Size, Layout, Style, TabCount };

  explicit TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TextSettings", renderer, mappedInput) {}

  static bool contains(StrId nameId);

  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  std::vector<SettingInfo> settings_;
  std::vector<FontEntry> fonts_;
  std::vector<uint8_t> sizeOptions_;
  int selectedTab_ = Font;
  int selectedRow_ = -1;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  int preparedPreviewFontId_ = 0;
  int customPreviewFontIndex_ = -1;
  uint8_t customPreviewFontSize_ = UINT8_MAX;
  bool customPreviewPending_ = false;
  std::unique_ptr<uint8_t[]> customPreviewSnapshot_;
  size_t customPreviewSnapshotSize_ = 0;
  std::vector<std::shared_ptr<TextBlock>> previewLines_;
  int previewTextX_ = 0;
  int previewTextY_ = 0;
  int previewTextBottom_ = 0;
  int previewLineHeight_ = 0;

  void rebuildSettings();
  void moveTab(int direction);
  void rebuildFontOptions();
  void rebuildSizeOptions();
  void moveSelection(int direction);
  void handleSelection();
  void applyFontSelection(int index);
  void applySizeSelection(int index);
  void invalidatePreviewLocked();
  void refreshPreviewAfterSettingChange(StrId settingId);
  void preparePreviewLines(int fontId, const char* text, int width, int maxLines);
  void drawPreparedPreview(int fontId) const;
  void renderPreviewPane(int top, int height, int fontId, const char* fontName, bool cachedCustomPreview,
                         bool prepareForAntiAliasing);
  int currentListSize() const;
  int currentFontIndex() const;
  int currentSizeIndex() const;
  std::string valueLabel(int index) const;
  std::string sizeLabel(int index) const;
  std::string selectedFontName() const;
  uint8_t selectedPointSize() const;
};
