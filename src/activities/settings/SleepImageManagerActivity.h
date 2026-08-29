#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/boot_sleep/SleepImageSelectionStore.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class SleepImageManagerActivity final : public Activity {
 public:
  enum class Mode { Manage, Placement };

  SleepImageManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode)
      : Activity(mode == Mode::Manage ? "SleepImageManager" : "SleepImagePlacementList", renderer, mappedInput),
        mode_(mode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Notice : uint8_t { None, Ready, Full, TooLarge, Invalid, IoError };

  Mode mode_;
  SleepImageSelectionStore::Catalog catalog_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  int selectedIndex_ = 0;
  Notice notice_ = Notice::None;

  int itemCount() const;
  void loadCatalog();
  void handleSelection();
  void openImagePicker();
  void addImage(const std::string& path);
  void confirmDelete(uint16_t id, const std::string& name);
  void showPlacementActions(uint16_t id, const std::string& name);
  void openPositionEditor(uint16_t id, bool zoom);
};
