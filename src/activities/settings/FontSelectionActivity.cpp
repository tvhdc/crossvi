#include "FontSelectionActivity.h"

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
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }

  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}

uint8_t currentEffectivePointSize(const SdCardFontRegistry* registry, const char* sdFontFamilyName,
                                  const uint8_t fontSize) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    if (const auto* family = registry->findFamily(sdFontFamilyName)) {
      if (const auto* selected = family->findClosestReaderSize(fontSize)) return selected->pointSize;
    }
  }
  return ReaderFontSize::pointSize(std::min<uint8_t>(fontSize, ReaderFontSize::BUILTIN_COUNT - 1));
}
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, const bool persistInvalidSelection)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      persistInvalidSelection_(persistInvalidSelection) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // The preview renders with SETTINGS.getReaderFontId(), which resolves to the
  // loaded SD font only after ensureLoaded() ran; without this the first frame
  // shows the built-in fallback font.
  sdFontSystem.ensureLoaded(renderer, persistInvalidSelection_);

  // Get metrics and calculate layout dimensions
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  originalFontFamily_ = SETTINGS.fontFamily;
  originalFontSize_ = SETTINGS.fontSize;
  strncpy(originalSdFontFamilyName_, SETTINGS.sdFontFamilyName, sizeof(originalSdFontFamilyName_) - 1);
  originalSdFontFamilyName_[sizeof(originalSdFontFamilyName_) - 1] = '\0';
  preferredPointSize_ = currentEffectivePointSize(registry_, originalSdFontFamilyName_, originalFontSize_);

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS)});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  previewFontIndex_ = selectedIndex_;

  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  // Finish on release so the same physical press cannot be observed by the
  // parent BookReaderSettingsActivity after this child is popped.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    {
      // A pending preview render may still be reading the currently loaded SD
      // font. Keep its buffers alive until that render has completed.
      RenderLock lock(*this);
      SETTINGS.fontFamily = originalFontFamily_;
      SETTINGS.fontSize = originalFontSize_;
      strncpy(SETTINGS.sdFontFamilyName, originalSdFontFamilyName_, sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer, persistInvalidSelection_);
    }
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewFontIndex_) {
      handleSelection();
    } else {
      previewFontIndex_ = selectedIndex_;
      applyFontSelection(selectedIndex_);
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  applyFontSelection(selectedIndex_);
  finish();
}

void FontSelectionActivity::applyFontSelection(const int index) {
  // Font preview rendering runs on ActivityManagerRender. Loading another SD
  // font unloads the current family, so serialize that ownership change with
  // any in-flight render that may still be using its glyph and kern buffers.
  RenderLock lock(*this);
  const auto& font = fonts_[index];
  if (font.settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.fontSize = ReaderFontSize::closestIndex(preferredPointSize_, ReaderFontSize::BUILTIN_COUNT);
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      const auto& family = families[sdIdx];
      const int logicalSize = family.findClosestReaderSizeEnum(preferredPointSize_);
      if (logicalSize < 0) return;
      SETTINGS.fontSize = static_cast<uint8_t>(logicalSize);
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    }
  }
  sdFontSystem.ensureLoaded(renderer, persistInvalidSelection_);
}

void FontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  const char* boldText = I18N.get(StrId::STR_FONT_PREVIEW_BOLD);
  const char* italicText = I18N.get(StrId::STR_FONT_PREVIEW_ITALIC);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s %s %s", previewText, boldText, italicText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x07);
  }

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  const auto regularLines = renderer.wrappedText(fontId, previewText, width, std::max(1, maxLines - 2));
  for (const auto& line : regularLines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
  if (y + lineH <= textBottomLimit) {
    renderer.drawText(fontId, left, y, boldText, true, EpdFontFamily::BOLD);
    y += lineH + 2;
  }
  if (y + lineH <= textBottomLimit) {
    renderer.drawText(fontId, left, y, italicText, true, EpdFontFamily::ITALIC);
  }

  if (SETTINGS.sdFontFamilyName[0] != '\0' && !sdFontSystem.currentSupportsVietnamese()) {
    renderer.drawText(labelFontId, left, labelY - labelH - 2, tr(STR_FONT_MISSING_VIETNAMESE));
  }
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_FONT_FAMILY));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  const int previewFontId = SETTINGS.getReaderFontId();
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewFontName);

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, pageWidth - 1, listTop - metrics_.verticalSpacing / 2);

  const int currentFontIndex = findCurrentFontIndex(registry_, originalSdFontFamilyName_, originalFontFamily_);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string {
        if (index == previewFontIndex_ && index != currentFontIndex) return tr(STR_PREVIEW);
        if (index == currentFontIndex) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewFontIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
}
