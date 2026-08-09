#include "HomeShortcutManagerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/home/HomeShortcutCatalog.h"
#include "components/UITheme.h"

void HomeShortcutManagerActivity::onEnter() {
  Activity::onEnter();
  mode_ = Mode::Manage;
  selectedIndex_ = 0;
  editingIndex_ = -1;
  requestUpdate();
}

int HomeShortcutManagerActivity::manageItemCount() const {
  const int configured = std::min<int>(SETTINGS.homeShortcuts.count, HomeShortcutList::CAPACITY);
  return configured + (configured < HomeShortcutList::CAPACITY ? 1 : 0);
}

int HomeShortcutManagerActivity::currentItemCount() const {
  return mode_ == Mode::Manage ? manageItemCount() : static_cast<int>(pickerItems_.size());
}

void HomeShortcutManagerActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (mode_ == Mode::Pick) {
      mode_ = Mode::Manage;
      selectedIndex_ =
          editingIndex_ >= 0 ? editingIndex_ : std::min<int>(SETTINGS.homeShortcuts.count, HomeShortcutList::CAPACITY);
      editingIndex_ = -1;
      pickerItems_.clear();
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mode_ == Mode::Pick) {
      choosePickerItem();
    } else if (selectedIndex_ < SETTINGS.homeShortcuts.count) {
      openActions();
    } else {
      openPicker(-1);
    }
    return;
  }

  const int count = currentItemCount();
  if (count <= 0) return;
  buttonNavigator_.onNext([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
}

void HomeShortcutManagerActivity::openActions() {
  if (selectedIndex_ < 0 || selectedIndex_ >= SETTINGS.homeShortcuts.count) return;
  actionLabels_.clear();
  actions_.clear();
  actionLabels_.push_back(StrId::STR_EDIT_SHORTCUT);
  actions_.push_back(RowAction::Edit);
  if (selectedIndex_ > 0) {
    actionLabels_.push_back(StrId::STR_MOVE_UP);
    actions_.push_back(RowAction::MoveUp);
  }
  if (selectedIndex_ + 1 < SETTINGS.homeShortcuts.count) {
    actionLabels_.push_back(StrId::STR_MOVE_DOWN);
    actions_.push_back(RowAction::MoveDown);
  }
  actionLabels_.push_back(StrId::STR_DELETE);
  actions_.push_back(RowAction::Delete);

  optionPopup_.show(StrId::STR_CUSTOMIZE_SHORTCUTS, actionLabels_.data(), static_cast<int>(actionLabels_.size()), 0,
                    [this](const int index) {
                      if (index >= 0 && index < static_cast<int>(actions_.size())) applyAction(actions_[index]);
                    });
  requestUpdate();
}

void HomeShortcutManagerActivity::applyAction(const RowAction action) {
  if (selectedIndex_ < 0 || selectedIndex_ >= SETTINGS.homeShortcuts.count) return;
  if (action == RowAction::Edit) {
    openPicker(selectedIndex_);
    return;
  }

  const HomeShortcutList previous = SETTINGS.homeShortcuts;
  switch (action) {
    case RowAction::MoveUp:
      if (SETTINGS.homeShortcuts.move(static_cast<uint8_t>(selectedIndex_), static_cast<uint8_t>(selectedIndex_ - 1))) {
        --selectedIndex_;
      }
      break;
    case RowAction::MoveDown:
      if (SETTINGS.homeShortcuts.move(static_cast<uint8_t>(selectedIndex_), static_cast<uint8_t>(selectedIndex_ + 1))) {
        ++selectedIndex_;
      }
      break;
    case RowAction::Delete:
      if (!SETTINGS.homeShortcuts.remove(static_cast<uint8_t>(selectedIndex_))) return;
      selectedIndex_ = std::min(selectedIndex_, std::max(0, manageItemCount() - 1));
      break;
    case RowAction::Edit:
      break;
  }
  persistOrRestore(previous);
  requestUpdate();
}

void HomeShortcutManagerActivity::openPicker(const int editingIndex) {
  editingIndex_ = editingIndex;
  pickerItems_.clear();
  pickerItems_.reserve(homeShortcutCatalog().size());
  for (const HomeShortcutDescriptor& descriptor : homeShortcutCatalog()) {
    if (!isHomeShortcutAvailable(descriptor.id, renderer)) continue;
    if (SETTINGS.homeShortcuts.contains(descriptor.id, editingIndex_)) continue;
    pickerItems_.push_back(descriptor.id);
  }
  if (pickerItems_.empty()) return;

  mode_ = Mode::Pick;
  selectedIndex_ = 0;
  if (editingIndex_ >= 0 && editingIndex_ < SETTINGS.homeShortcuts.count) {
    const HomeShortcutId current = SETTINGS.homeShortcuts.at(editingIndex_);
    const auto it = std::find(pickerItems_.begin(), pickerItems_.end(), current);
    if (it != pickerItems_.end()) selectedIndex_ = static_cast<int>(std::distance(pickerItems_.begin(), it));
  }
  requestUpdate();
}

void HomeShortcutManagerActivity::choosePickerItem() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(pickerItems_.size())) return;
  const HomeShortcutList previous = SETTINGS.homeShortcuts;
  bool changed = false;
  if (editingIndex_ >= 0) {
    changed = SETTINGS.homeShortcuts.replace(static_cast<uint8_t>(editingIndex_), pickerItems_[selectedIndex_]);
  } else {
    changed = SETTINGS.homeShortcuts.add(pickerItems_[selectedIndex_]);
  }
  if (!changed) return;
  persistOrRestore(previous);
  const int returnIndex = editingIndex_ >= 0 ? editingIndex_ : SETTINGS.homeShortcuts.count - 1;
  mode_ = Mode::Manage;
  editingIndex_ = -1;
  pickerItems_.clear();
  selectedIndex_ = std::clamp(returnIndex, 0, std::max(0, manageItemCount() - 1));
  requestUpdate();
}

void HomeShortcutManagerActivity::persistOrRestore(const HomeShortcutList& previous) {
  if (!SETTINGS.saveToFile()) SETTINGS.homeShortcuts = previous;
}

std::string HomeShortcutManagerActivity::manageRowLabel(const int index) const {
  if (index >= 0 && index < SETTINGS.homeShortcuts.count) {
    const auto* descriptor = findHomeShortcut(SETTINGS.homeShortcuts.at(static_cast<uint8_t>(index)));
    return descriptor ? std::string(I18N.get(descriptor->label)) : std::string();
  }
  return I18N.get(StrId::STR_ADD_SHORTCUT);
}

void HomeShortcutManagerActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const StrId title = mode_ == Mode::Pick ? (editingIndex_ >= 0 ? StrId::STR_EDIT_SHORTCUT : StrId::STR_ADD_SHORTCUT)
                                          : StrId::STR_CUSTOMIZE_SHORTCUTS;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, I18N.get(title));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int count = currentItemCount();
  GUI.drawList(renderer, Rect{0, contentTop, width, contentHeight}, count, selectedIndex_, [this](const int index) {
    if (mode_ == Mode::Manage) return manageRowLabel(index);
    const auto* descriptor = findHomeShortcut(pickerItems_[index]);
    return descriptor ? std::string(I18N.get(descriptor->label)) : std::string();
  });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
