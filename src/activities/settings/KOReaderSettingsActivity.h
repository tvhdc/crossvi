#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, and authenticate options.
 */
class KOReaderSettingsActivity final : public Activity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KOReaderSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;

  size_t selectedIndex = 0;
  bool showSaveError = false;

  void handleSelection();
  void showServerPicker();
  void showCustomServerActions(size_t customIndex);
  void openCustomServerEditor(size_t customIndex);
  void openNewServerEditor();
  void confirmCustomServerDelete(size_t customIndex);
  void reportSaveResult(bool saved);
};
