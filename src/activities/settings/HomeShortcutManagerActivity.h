#pragma once

#include <vector>

#include "HomeShortcuts.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class HomeShortcutManagerActivity final : public Activity {
 public:
  explicit HomeShortcutManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HomeShortcutManager", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode { Manage, Pick };
  enum class RowAction { Edit, MoveUp, MoveDown, Delete };

  Mode mode_ = Mode::Manage;
  int selectedIndex_ = 0;
  int editingIndex_ = -1;
  std::vector<HomeShortcutId> pickerItems_;
  std::vector<StrId> actionLabels_;
  std::vector<RowAction> actions_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;

  int manageItemCount() const;
  int currentItemCount() const;
  void openActions();
  void applyAction(RowAction action);
  void openPicker(int editingIndex);
  void choosePickerItem();
  void persistOrRestore(const HomeShortcutList& previous);
  std::string manageRowLabel(int index) const;
};
