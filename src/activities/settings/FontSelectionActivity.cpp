#include "FontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSize.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
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

  // Keep .cpfont data reader-owned. The picker loads it only long enough to
  // render and snapshot the real preview, then releases it again.
  sdFontSystem.releaseLoadedFont(renderer);
  // Preserve ensureLoaded()'s old missing-family repair without loading any
  // font file. This only consults the already-discovered registry.
  if (SETTINGS.sdFontFamilyName[0] != '\0' &&
      (!registry_ || (registry_->lastDiscoverySucceeded() && !registry_->findFamily(SETTINGS.sdFontFamilyName)))) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    if (SETTINGS.fontSize >= ReaderFontSize::BUILTIN_COUNT) SETTINGS.fontSize = CrossPointSettings::EXTRA_LARGE;
    if (persistInvalidSelection_) SETTINGS.saveToFile();
  }

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

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  previewFontIndex_ = selectedIndex_;
  preparedPreviewFontId_ = 0;
  customPreviewAttemptedIndex_ = -1;
  customPreviewSnapshotIndex_ = -1;
  customPreviewPending_ = selectedIndex_ >= CrossPointSettings::BUILTIN_FONT_COUNT;
  customPreviewSnapshot_.reset();
  customPreviewSnapshotSize_ = 0;

  if (customPreviewPending_) showBlockingFeedback(StrId::STR_LOADING_FONT_PREVIEW);
  requestUpdate();
}

void FontSelectionActivity::onExit() {
  sdFontSystem.releaseLoadedFont(renderer);
  customPreviewSnapshot_.reset();
  customPreviewSnapshotSize_ = 0;
  if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
  Activity::onExit();
}

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
      sdFontSystem.releaseLoadedFont(renderer);
    }
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) &&
      selectedIndex_ >= CrossPointSettings::BUILTIN_FONT_COUNT && selectedIndex_ != previewFontIndex_) {
    queueBlockingFeedback(StrId::STR_LOADING_FONT_PREVIEW);
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool selectedSdFont = selectedIndex_ >= CrossPointSettings::BUILTIN_FONT_COUNT;
    bool previewWasAttempted = false;
    {
      RenderLock lock(*this);
      previewWasAttempted = customPreviewAttemptedIndex_ == selectedIndex_;
    }
    if (selectedIndex_ == previewFontIndex_ && (!selectedSdFont || previewWasAttempted)) {
      handleSelection();
    } else {
      applyFontSelection(selectedIndex_, true);
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease(
      [this, listSize] { selectIndex(ButtonNavigator::nextIndex(selectedIndex_, listSize)); });

  buttonNavigator_.onPreviousRelease(
      [this, listSize] { selectIndex(ButtonNavigator::previousIndex(selectedIndex_, listSize)); });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectIndex(ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems));
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectIndex(ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems));
  });
}

void FontSelectionActivity::selectIndex(const int index) {
  if (index == selectedIndex_) return;
  selectedIndex_ = index;
  requestUpdate();
}

void FontSelectionActivity::handleSelection() {
  applyFontSelection(selectedIndex_);
  finish();
}

void FontSelectionActivity::applyFontSelection(const int index, const bool preparePreview) {
  // Serialize settings changes with a possible in-flight preview render. SD
  // font data remains unloaded until the active reader resumes.
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
  sdFontSystem.releaseLoadedFont(renderer);
  if (preparePreview) {
    previewFontIndex_ = index;
    const bool sdFont = index >= CrossPointSettings::BUILTIN_FONT_COUNT;
    customPreviewPending_ = sdFont;
    customPreviewAttemptedIndex_ = sdFont ? -1 : customPreviewAttemptedIndex_;
    customPreviewSnapshotIndex_ = -1;
    customPreviewSnapshot_.reset();
    customPreviewSnapshotSize_ = 0;
    preparedPreviewFontId_ = 0;
  }
}

void FontSelectionActivity::renderPreviewPane(const int top, const int height, const int fontId, const char* fontName,
                                              const bool cachedCustomPreview) {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  if (fontId == 0 && !cachedCustomPreview) {
    snprintf(labelBuf, sizeof(labelBuf), "%s - %s", fontName ? fontName : "", tr(STR_IN_READER));
  } else {
    snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  }
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) {
    if (!cachedCustomPreview && fontName && fontName[0] != '\0') {
      renderer.drawText(UI_12_FONT_ID, left, top + metrics_.previewPadding, fontName, true, EpdFontFamily::BOLD);
    }
    return;
  }

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (preparedPreviewFontId_ != fontId) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, previewText, 0x01);
    }
    preparedPreviewFontId_ = fontId;
  }

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  const auto regularLines = renderer.wrappedText(fontId, previewText, width, maxLines);
  for (const auto& line : regularLines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }

  // SD fonts stay unloaded in Settings by design, so their glyph coverage
  // cannot be judged here. Reporting the unloaded manager as unsupported
  // produced a false "missing Vietnamese" warning for valid custom fonts.
}

void FontSelectionActivity::render(RenderLock&&) {
  if (renderBlockingFeedbackOverlay()) return;
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_FONT_FAMILY));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  const bool sdPreview = previewFontIndex_ >= CrossPointSettings::BUILTIN_FONT_COUNT;
  int previewFontId = sdPreview ? 0 : SETTINGS.getReaderFontId();
  if (sdPreview && customPreviewPending_) {
    // Explicit preview only: temporarily load the selected .cpfont while this
    // render owns the framebuffer, then snapshot the pixels and unload it.
    sdFontSystem.ensureLoaded(renderer, false);
    previewFontId = SETTINGS.getReaderFontId();
  }
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  bool cachedCustomPreview = sdPreview && customPreviewSnapshot_ && customPreviewSnapshotIndex_ == previewFontIndex_ &&
                             customPreviewSnapshotSize_ > 0;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewFontName, cachedCustomPreview);

  const int previewTextX = metrics_.previewPadding;
  const int previewTextY = previewTop + metrics_.previewPadding;
  const int previewTextW = pageWidth - (metrics_.previewPadding * 2);
  const int previewLabelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int previewTextH = previewHeight - metrics_.previewPadding - (previewLabelH + 4 + metrics_.previewPadding);

  if (cachedCustomPreview && !renderer.copyBufferToRegion(previewTextX, previewTextY, previewTextW, previewTextH,
                                                          customPreviewSnapshot_.get(), customPreviewSnapshotSize_)) {
    customPreviewSnapshot_.reset();
    customPreviewSnapshotSize_ = 0;
    customPreviewSnapshotIndex_ = -1;
    cachedCustomPreview = false;
    renderPreviewPane(previewTop, previewHeight, 0, previewFontName);
  }

  if (sdPreview && customPreviewPending_) {
    if (previewFontId != 0) {
      const size_t snapshotSize = renderer.getRegionByteSize(previewTextX, previewTextY, previewTextW, previewTextH);
      std::unique_ptr<uint8_t[]> snapshot(snapshotSize > 0 ? new (std::nothrow) uint8_t[snapshotSize] : nullptr);
      if (snapshot && renderer.copyRegionToBuffer(previewTextX, previewTextY, previewTextW, previewTextH,
                                                  snapshot.get(), snapshotSize)) {
        customPreviewSnapshot_ = std::move(snapshot);
        customPreviewSnapshotSize_ = snapshotSize;
        customPreviewSnapshotIndex_ = previewFontIndex_;
      }
    }
    customPreviewAttemptedIndex_ = previewFontIndex_;
    customPreviewPending_ = false;
    preparedPreviewFontId_ = 0;
    sdFontSystem.releaseLoadedFont(renderer);
  }

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

  const bool selectedSdFont = selectedIndex_ >= CrossPointSettings::BUILTIN_FONT_COUNT;
  const bool onPreviewed =
      selectedIndex_ == previewFontIndex_ && (!selectedSdFont || customPreviewAttemptedIndex_ == selectedIndex_);
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
