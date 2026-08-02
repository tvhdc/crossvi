#include "FontSizeSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSize.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

FontSizeSelectionActivity::FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontSizeSelect", renderer, mappedInput) {}

void FontSizeSelectionActivity::onEnter() {
  Activity::onEnter();
  // The preview renders with SETTINGS.getReaderFontId(); load the selected SD
  // font before the first frame so it is previewed instead of the built-in
  // fallback.
  sdFontSystem.ensureLoaded(renderer, false);
  metrics_ = UITheme::getInstance().getMetrics();
  originalFontFamily_ = SETTINGS.fontFamily;
  originalSize_ =
      SETTINGS.fontSize < CrossPointSettings::FONT_SIZE_COUNT ? SETTINGS.fontSize : CrossPointSettings::MEDIUM;
  std::strncpy(originalSdFontFamilyName_, SETTINGS.sdFontFamilyName, sizeof(originalSdFontFamilyName_) - 1);
  originalSdFontFamilyName_[sizeof(originalSdFontFamilyName_) - 1] = '\0';
  buildSizeOptions();
  const auto selected = std::find(sizeOptions_.begin(), sizeOptions_.end(), originalSize_);
  if (selected != sizeOptions_.end()) {
    selectedIndex_ = static_cast<int>(std::distance(sizeOptions_.begin(), selected));
  } else {
    const uint8_t target = ReaderFontSize::pointSize(originalSize_);
    uint8_t bestDelta = UINT8_MAX;
    selectedIndex_ = 0;
    for (int index = 0; index < static_cast<int>(sizeOptions_.size()); ++index) {
      const uint8_t candidate = ReaderFontSize::pointSize(sizeOptions_[index]);
      const uint8_t delta = candidate > target ? candidate - target : target - candidate;
      if (delta < bestDelta) {
        selectedIndex_ = index;
        bestDelta = delta;
      }
    }
  }
  requestUpdate();
}

void FontSizeSelectionActivity::buildSizeOptions() {
  sizeOptions_.clear();
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    if (const auto* family = sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName)) {
      sizeOptions_ = family->availableReaderSizeEnums();
    }
  }
  if (sizeOptions_.empty()) {
    for (uint8_t index = 0; index < ReaderFontSize::BUILTIN_COUNT; ++index) sizeOptions_.push_back(index);
  }
}

void FontSizeSelectionActivity::previewSelection(const int index) {
  // Changing point size can unload the SD font currently used by an in-flight
  // preview render. Wait for it before replacing the font buffers.
  RenderLock lock(*this);
  selectedIndex_ = std::clamp(index, 0, static_cast<int>(sizeOptions_.size()) - 1);
  SETTINGS.fontSize = sizeOptions_[selectedIndex_];
  sdFontSystem.ensureLoaded(renderer, false);
  requestUpdate();
}

void FontSizeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    {
      RenderLock lock(*this);
      SETTINGS.fontFamily = originalFontFamily_;
      SETTINGS.fontSize = originalSize_;
      std::strncpy(SETTINGS.sdFontFamilyName, originalSdFontFamilyName_, sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer, false);
    }
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }
  // Finish on release so the same edge cannot be observed by Settings after
  // this child activity is popped.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(ActivityResult{});
    finish();
    return;
  }

  buttonNavigator_.onNextRelease(
      [this] { previewSelection(ButtonNavigator::nextIndex(selectedIndex_, static_cast<int>(sizeOptions_.size()))); });
  buttonNavigator_.onPreviousRelease([this] {
    previewSelection(ButtonNavigator::previousIndex(selectedIndex_, static_cast<int>(sizeOptions_.size())));
  });
}

std::string FontSizeSelectionActivity::sizeLabel(const int index) const {
  char label[16];
  const uint8_t logicalSize = sizeOptions_[index];
  const uint8_t actual = SETTINGS.sdFontFamilyName[0] == '\0'
                             ? ReaderFontSize::pointSize(logicalSize)
                             : sdFontSystem.selectedPointSize(SETTINGS.sdFontFamilyName, logicalSize);
  snprintf(label, sizeof(label), "%u pt",
           static_cast<unsigned>(actual ? actual : ReaderFontSize::pointSize(logicalSize)));
  return label;
}

std::string FontSizeSelectionActivity::actualSizeLabel(const int index) const {
  (void)index;
  // The primary label already shows the physical size that will be rendered.
  return {};
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

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, screenWidth - 1, listTop - metrics_.verticalSpacing / 2);
  GUI.drawList(
      renderer, Rect{0, listTop, screenWidth, std::max(0, screenHeight - bottomReserved - listTop)},
      static_cast<int>(sizeOptions_.size()), selectedIndex_, [this](const int index) { return sizeLabel(index); },
      nullptr, nullptr, [this](const int index) { return actualSizeLabel(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
}
