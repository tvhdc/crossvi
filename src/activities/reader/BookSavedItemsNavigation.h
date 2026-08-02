#pragma once

#include <algorithm>
#include <cstdint>

namespace BookSavedItemsNavigation {

struct State {
  bool tabFocused = true;
  uint16_t selectedItem = 0;
};

enum class BackAction : uint8_t { FocusTab, Exit };

inline uint16_t clampItem(const uint16_t item, const uint16_t itemCount) {
  return itemCount == 0 ? 0 : std::min<uint16_t>(item, static_cast<uint16_t>(itemCount - 1));
}

inline State restore(State state, const uint16_t itemCount) {
  if (itemCount == 0) {
    state.tabFocused = true;
    state.selectedItem = 0;
    return state;
  }
  state.selectedItem = clampItem(state.selectedItem, itemCount);
  return state;
}

inline bool next(State& state, const uint16_t itemCount) {
  state = restore(state, itemCount);
  if (state.tabFocused) {
    if (itemCount == 0) return false;
    state.tabFocused = false;
    state.selectedItem = 0;
    return true;
  }
  if (state.selectedItem + 1 >= itemCount) {
    state.tabFocused = true;
    return true;
  }
  ++state.selectedItem;
  return true;
}

inline bool previous(State& state, const uint16_t itemCount) {
  state = restore(state, itemCount);
  if (state.tabFocused) return false;
  if (state.selectedItem == 0) {
    state.tabFocused = true;
    return true;
  }
  --state.selectedItem;
  return true;
}

inline BackAction back(State& state, const uint16_t itemCount) {
  state = restore(state, itemCount);
  if (!state.tabFocused) {
    state.tabFocused = true;
    return BackAction::FocusTab;
  }
  return BackAction::Exit;
}

inline uint8_t clampTab(const uint8_t tab, const uint8_t tabCount) {
  return tabCount == 0 ? 0 : std::min<uint8_t>(tab, static_cast<uint8_t>(tabCount - 1));
}

inline uint8_t nextTab(const uint8_t tab, const uint8_t tabCount) {
  if (tabCount <= 1) return 0;
  return static_cast<uint8_t>((clampTab(tab, tabCount) + 1) % tabCount);
}

inline uint8_t previousTab(const uint8_t tab, const uint8_t tabCount) {
  if (tabCount <= 1) return 0;
  return static_cast<uint8_t>((clampTab(tab, tabCount) + tabCount - 1) % tabCount);
}

}  // namespace BookSavedItemsNavigation
