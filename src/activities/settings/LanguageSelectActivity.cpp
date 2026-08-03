#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  firstLanguagePosition = (it != end) ? static_cast<uint8_t>(std::distance(begin, it)) : 0;
  selectedIndex = 0;

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    saveFailed = false;
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    saveFailed = false;
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    saveFailed = false;
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    saveFailed = false;
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  const uint8_t langIndex = languageAt(selectedIndex);
  const auto selectedLanguage = static_cast<Language>(langIndex);

  if (SETTINGS.language == langIndex && I18N.getLanguage() == selectedLanguage) {
    onBack();
    return;
  }

  const uint8_t previousLanguage = SETTINGS.language;
  SETTINGS.language = langIndex;
  if (!SETTINGS.saveToFile()) {
    SETTINGS.language = previousLanguage;
    saveFailed = true;
    LOG_ERR("LANG", "Could not persist selected language");
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    I18N.setLanguage(selectedLanguage);
  }

  // Return to previous page
  onBack();
}

uint8_t LanguageSelectActivity::languageAt(int index) const {
  const int sortedIndex = (static_cast<int>(firstLanguagePosition) + index) % totalItems;
  return SORTED_LANGUAGE_INDICES[sortedIndex];
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  // Current language marker
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int index) { return I18N.getLanguageName(static_cast<Language>(languageAt(index))); }, nullptr, nullptr,
      [this, currentLang](int index) { return languageAt(index) == currentLang ? tr(STR_SELECTED) : ""; }, true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (saveFailed) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));

  renderer.displayBuffer();
}
