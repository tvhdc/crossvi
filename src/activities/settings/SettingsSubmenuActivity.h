#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "SettingsActivity.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryRegistry.h"

class SettingsSubmenuActivity final : public Activity {
 public:
  enum class Page {
    HomeLibrary,
    Sleep,
    Dictionary,
    PageButtons,
    ConfirmButton,
    PowerButton,
    FirmwareUpdate,
  };

  SettingsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Page page,
                          std::vector<DictionaryEntry> dictionaries = {})
      : Activity("SettingsSubmenu", renderer, mappedInput), page_(page), dictionaries_(std::move(dictionaries)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Page page_;
  std::vector<DictionaryEntry> dictionaries_;
  std::vector<SettingInfo> settings_;
  int selectedIndex_ = 0;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  enum class SleepImageNotice : uint8_t { None, Ready, Optimized, TooLarge, Invalid, IoError };
  SleepImageNotice sleepImageNotice_ = SleepImageNotice::None;
  uint64_t sleepImageSourceBytes_ = 0;
  uint64_t sleepImageOutputBytes_ = 0;

  StrId title() const;
  void rebuildSettings();
  void handleSelection();
  void openSleepTimeoutPicker();
  void showSleepImageDialog(uint8_t mode);
  void openSleepImagePicker(uint8_t mode);
  void applySleepImageSelection(uint8_t mode, const std::string& path);
  void openAction(SettingAction action);
  std::string valueLabel(int index) const;
};
