#include "FontSizeSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace {
constexpr uint8_t kPointSizes[CrossPointSettings::FONT_SIZE_COUNT] = {12, 14, 16, 18};
constexpr StrId kSizeLabels[CrossPointSettings::FONT_SIZE_COUNT] = {
    StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
}  // namespace

FontSizeSelectionActivity::FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const bool persistInvalidSelection)
    : Activity("FontSizeSelect", renderer, mappedInput), persistInvalidSelection_(persistInvalidSelection) {}

void FontSizeSelectionActivity::onEnter() {
  Activity::onEnter();
  metrics_ = UITheme::getInstance().getMetrics();
  originalSize_ = SETTINGS.fontSize < CrossPointSettings::FONT_SIZE_COUNT ? SETTINGS.fontSize
                                                                          : CrossPointSettings::MEDIUM;
  selectedIndex_ = originalSize_;
  requestUpdate();
}

void FontSizeSelectionActivity::previewSelection(const int index) {
  selectedIndex_ = std::clamp(index, 0, static_cast<int>(CrossPointSettings::FONT_SIZE_COUNT) - 1);
  SETTINGS.fontSize = static_cast<uint8_t>(selectedIndex_);
  sdFontSystem.ensureLoaded(renderer, persistInvalidSelection_);
  requestUpdate();
}

void FontSizeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.fontSize = originalSize_;
    sdFontSystem.ensureLoaded(renderer, persistInvalidSelection_);
    finish();
    return;
  }
  // Finish on release so the same edge cannot be observed by Settings after
  // this child activity is popped.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    SETTINGS.fontSize = static_cast<uint8_t>(selectedIndex_);
    finish();
    return;
  }

  buttonNavigator_.onNextRelease([this] {
    previewSelection(ButtonNavigator::nextIndex(selectedIndex_, CrossPointSettings::FONT_SIZE_COUNT));
  });
  buttonNavigator_.onPreviousRelease([this] {
    previewSelection(ButtonNavigator::previousIndex(selectedIndex_, CrossPointSettings::FONT_SIZE_COUNT));
  });
}

std::string FontSizeSelectionActivity::sizeLabel(const int index) const {
  char label[64];
  snprintf(label, sizeof(label), "%s — %u pt", I18N.get(kSizeLabels[index]), kPointSizes[index]);
  return label;
}

std::string FontSizeSelectionActivity::actualSizeLabel(const int index) const {
  if (SETTINGS.sdFontFamilyName[0] == '\0') return {};
  const uint8_t actual = sdFontSystem.selectedPointSize(SETTINGS.sdFontFamilyName, static_cast<uint8_t>(index));
  if (actual == 0 || actual == kPointSizes[index]) return {};
  char label[48];
  snprintf(label, sizeof(label), tr(STR_FONT_ACTUAL_SIZE_FORMAT), actual);
  return label;
}

void FontSizeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int contentTop = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  const int bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  const int usableHeight = std::max(0, screenHeight - contentTop - bottomReserved);
  const int previewHeight = std::max(70, usableHeight * metrics_.previewHeightPercent / 100);
  const int listTop = contentTop + previewHeight + metrics_.verticalSpacing;

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, screenWidth, metrics_.headerHeight}, tr(STR_FONT_SIZE));

  const int fontId = SETTINGS.getReaderFontId();
  const char* preview = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (fontId != 0) {
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
      cache->prewarmCache(fontId, preview, 0x01);
    }
    const int left = metrics_.previewPadding;
    const int width = screenWidth - 2 * left;
    const int lineHeight = renderer.getTextHeight(fontId) + 2;
    const int maxLines = std::max(1, (previewHeight - metrics_.previewPadding * 2) / std::max(1, lineHeight));
    const auto lines = renderer.wrappedText(fontId, preview, width, maxLines);
    int y = contentTop + metrics_.previewPadding;
    for (const auto& line : lines) {
      if (y + lineHeight > contentTop + previewHeight) break;
      renderer.drawText(fontId, left, y, line.c_str());
      y += lineHeight;
    }
  }

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, screenWidth - 1,
                    listTop - metrics_.verticalSpacing / 2);
  GUI.drawList(
      renderer, Rect{0, listTop, screenWidth, std::max(0, screenHeight - bottomReserved - listTop)},
      CrossPointSettings::FONT_SIZE_COUNT, selectedIndex_, [this](const int index) { return sizeLabel(index); },
      nullptr, nullptr, [this](const int index) { return actualSizeLabel(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
}
