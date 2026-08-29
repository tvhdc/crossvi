#pragma once

#include <vector>

#include "HomeShortcutCatalog.h"
#include "activities/Activity.h"
#include "activities/home/HomeMenuMapping.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct SettingInfo;

class HomeShortcutsActivity final : public Activity {
 public:
  HomeShortcutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HomeMenuItem returnMenuItem)
      : Activity("HomeShortcuts", renderer, mappedInput), returnMenuItem_(returnMenuItem) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const HomeMenuItem returnMenuItem_;
  std::vector<HomeShortcutId> items_;
  int selectedIndex_ = 0;
  bool showSaveError_ = false;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;

  void rebuildItems();
  void activateSelected();
  void activateSetting(const HomeShortcutDescriptor& descriptor);
  void openScreen(HomeShortcutTarget target);
  const SettingInfo* findSetting(const char* key) const;
  std::string valueLabel(HomeShortcutId id) const;
};
