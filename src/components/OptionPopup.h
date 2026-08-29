#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex, std::function<void(int)> onSelect,
            const char* footerStr = nullptr) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    footer = footerStr ? footerStr : "";
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect, const char* footerStr = nullptr) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    footer = footerStr ? footerStr : "";
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex, std::function<void(int)> onSelect,
            const char* footerStr = nullptr) {
    title = I18N.get(titleId);
    ownedStrings = options;
    footer = footerStr ? footerStr : "";
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  void show(StrId titleId, std::vector<std::string>&& options, int currentIndex, std::function<void(int)> onSelect,
            const char* footerStr = nullptr) {
    title = I18N.get(titleId);
    ownedStrings = std::move(options);
    footer = footerStr ? footerStr : "";
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  bool handleInput(const MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    navigator.onPrevious([this, &requestUpdate] { moveSelection(-1, requestUpdate); });
    navigator.onNext([this, &requestUpdate] { moveSelection(1, requestUpdate); });
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      return selectCurrent(requestUpdate);
    } else if (input.wasReleased(MappedInputManager::Button::Back)) {
      return dismiss(requestUpdate);
    }
    return true;
  }

  bool moveSelection(const int delta, const std::function<void()>& requestUpdate) {
    if (!active || ownedStrings.empty() || delta == 0) return false;
    const int count = static_cast<int>(ownedStrings.size());
    selectedIndex = ((selectedIndex + delta) % count + count) % count;
    requestUpdate();
    return true;
  }

  bool selectCurrent(const std::function<void()>& requestUpdate) {
    if (!active) return false;
    active = false;
    if (onSelectCallback) onSelectCallback(selectedIndex);
    requestUpdate();
    return true;
  }

  bool dismiss(const std::function<void()>& requestUpdate) {
    if (!active) return false;
    active = false;
    requestUpdate();
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex, footer.c_str());
  }

  bool isActive() const { return active; }

 private:
  bool active = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  std::string footer;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  ButtonNavigator navigator;
};
