#pragma once

#include <vector>

#include "HomeShortcutCatalog.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct SettingInfo;

class HomeShortcutsActivity final : public Activity {
 public:
  explicit HomeShortcutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HomeShortcuts", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<HomeShortcutId> items_;
  int selectedIndex_ = 0;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;

  void rebuildItems();
  void activateSelected();
  void activateSetting(const HomeShortcutDescriptor& descriptor);
  void openScreen(HomeShortcutTarget target);
  const SettingInfo* findSetting(const char* key) const;
  std::string valueLabel(HomeShortcutId id) const;
};
