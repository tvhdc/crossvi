#include "TextSettingsActivity.h"

#include <Epub/ParsedText.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>

#include "CrossPointSettings.h"
#include "ReaderFontSize.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr StrId TAB_LABELS[TextSettingsActivity::TabCount] = {
    StrId::STR_TEXT_TAB_FONT,
    StrId::STR_TEXT_TAB_SIZE,
    StrId::STR_TEXT_TAB_LAYOUT,
    StrId::STR_TEXT_TAB_STYLE,
};

constexpr int PREVIEW_TO_TABS_GAP = 8;

bool settingAffectsTextPreview(const StrId settingId) {
  switch (settingId) {
    case StrId::STR_LINE_SPACING:
    case StrId::STR_WORD_SPACING:
    case StrId::STR_SCREEN_MARGIN:
    case StrId::STR_PARA_ALIGNMENT:
    case StrId::STR_EXTRA_SPACING:
    case StrId::STR_FORCE_PARAGRAPH_INDENTS:
    case StrId::STR_EMBEDDED_STYLE:
    case StrId::STR_HYPHENATION:
    case StrId::STR_READER_DARK_MODE:
    case StrId::STR_FOCUS_READING:
    case StrId::STR_TEXT_AA:
      return true;
    default:
      return false;
  }
}

uint8_t effectivePointSize(const SdCardFontRegistry& registry, const char* familyName, const uint8_t logicalSize) {
  if (familyName[0] != '\0') {
    if (const auto* family = registry.findFamily(familyName)) {
      if (const auto* selected = family->findClosestReaderSize(logicalSize)) return selected->pointSize;
    }
  }
  return ReaderFontSize::pointSize(std::min<uint8_t>(logicalSize, ReaderFontSize::BUILTIN_COUNT - 1));
}
}  // namespace

bool TextSettingsActivity::contains(const StrId nameId) {
  switch (nameId) {
    case StrId::STR_FONT_FAMILY:
    case StrId::STR_FONT_SIZE:
    case StrId::STR_TEXT_AA:
    case StrId::STR_READER_DARK_MODE:
    case StrId::STR_LINE_SPACING:
    case StrId::STR_WORD_SPACING:
    case StrId::STR_SCREEN_MARGIN:
    case StrId::STR_EMBEDDED_STYLE:
    case StrId::STR_PARA_ALIGNMENT:
    case StrId::STR_EXTRA_SPACING:
    case StrId::STR_FORCE_PARAGRAPH_INDENTS:
    case StrId::STR_HYPHENATION:
    case StrId::STR_FOCUS_READING:
      return true;
    default:
      return false;
  }
}

void TextSettingsActivity::persistSettings() { settingsSavePending_ = !SETTINGS.saveToFile(); }

void TextSettingsActivity::rebuildFontOptions() {
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilyCount());
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  const auto& families = sdFontSystem.registry().getFamilies();
  for (int index = 0; index < static_cast<int>(families.size()); ++index) {
    fonts_.push_back(
        {families[index].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + index)});
  }
}

void TextSettingsActivity::rebuildSizeOptions() {
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

void TextSettingsActivity::rebuildSettings() {
  settings_.clear();
  switch (selectedTab_) {
    case Font:
    case Size:
      break;
    case Layout:
      settings_.push_back(buildScreenMarginSetting());
      settings_.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                            {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}));
      settings_.push_back(SettingInfo::Enum(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                                            {StrId::STR_NORMAL, StrId::STR_WORD_SPACING_1, StrId::STR_WORD_SPACING_2,
                                             StrId::STR_WORD_SPACING_3, StrId::STR_WORD_SPACING_4}));
      settings_.push_back(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                                            {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                             StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE}));
      if (!SETTINGS.embeddedStyle) {
        SettingInfo& alignment = settings_.back();
        alignment.enumValues.pop_back();
        alignment.valuePtr = nullptr;
        alignment.valueGetter = [] {
          return SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE
                     ? static_cast<uint8_t>(CrossPointSettings::JUSTIFIED)
                     : SETTINGS.paragraphAlignment;
        };
        alignment.valueSetter = [](const uint8_t value) { SETTINGS.paragraphAlignment = value; };
      }
      settings_.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing));
      settings_.push_back(
          SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents));
      settings_.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle));
      settings_.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled));
      break;
    case Style:
      settings_.push_back(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode));
      settings_.push_back(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled));
      if (renderer.supportsStripGrayscale()) {
        settings_.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing));
      }
      break;
    default:
      break;
  }
  if (selectedRow_ >= currentListSize()) selectedRow_ = currentListSize() - 1;
}

void TextSettingsActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.refreshIfDirty();
  sdFontSystem.releaseLoadedFont(renderer);

  if (SETTINGS.sdFontFamilyName[0] != '\0' && !sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName)) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    if (SETTINGS.fontSize >= ReaderFontSize::BUILTIN_COUNT) {
      SETTINGS.fontSize = CrossPointSettings::EXTRA_LARGE;
    }
    persistSettings();
  }

  rebuildFontOptions();
  rebuildSizeOptions();
  selectedTab_ = Font;
  selectedRow_ = -1;
  preparedPreviewFontId_ = 0;
  customPreviewFontIndex_ = -1;
  customPreviewFontSize_ = UINT8_MAX;
  customPreviewSnapshot_.reset();
  customPreviewSnapshotSize_ = 0;
  customPreviewPending_ = SETTINGS.sdFontFamilyName[0] != '\0';
  rebuildSettings();
  if (customPreviewPending_) showBlockingFeedback(StrId::STR_LOADING_FONT_PREVIEW);
  requestUpdate();
}

void TextSettingsActivity::onExit() {
  if (settingsSavePending_) persistSettings();
  sdFontSystem.releaseLoadedFont(renderer);
  customPreviewSnapshot_.reset();
  customPreviewSnapshotSize_ = 0;
  if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
  Activity::onExit();
}

void TextSettingsActivity::onResume() {
  // This screen no longer opens child font/size pickers. Keep this hook for
  // Activity compatibility and ensure a resumed screen owns no SD font data.
  sdFontSystem.releaseLoadedFont(renderer);
}

int TextSettingsActivity::currentListSize() const {
  switch (selectedTab_) {
    case Font:
      return static_cast<int>(fonts_.size());
    case Size:
      return static_cast<int>(sizeOptions_.size());
    case Layout:
    case Style:
      return static_cast<int>(settings_.size());
    default:
      return 0;
  }
}

void TextSettingsActivity::moveSelection(const int direction) {
  const int count = currentListSize() + 1;
  const int current = selectedRow_ + 1;
  const int next =
      direction > 0 ? ButtonNavigator::nextIndex(current, count) : ButtonNavigator::previousIndex(current, count);
  selectedRow_ = next - 1;
  requestUpdate();
}

void TextSettingsActivity::moveTab(const int direction) {
  selectedTab_ = direction < 0 ? ButtonNavigator::previousIndex(selectedTab_, TabCount)
                               : ButtonNavigator::nextIndex(selectedTab_, TabCount);
  rebuildSettings();
  requestUpdate();
}

void TextSettingsActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedRow_ >= 0) {
      selectedRow_ = -1;
      requestUpdate();
    } else {
      if (settingsSavePending_) persistSettings();
      finish();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && selectedRow_ >= 0) {
    bool needsCustomPreview = false;
    if (selectedTab_ == Font && selectedRow_ < static_cast<int>(fonts_.size()) && selectedRow_ != currentFontIndex()) {
      needsCustomPreview = !fonts_[selectedRow_].isBuiltin;
    } else if (selectedTab_ == Size && selectedRow_ < static_cast<int>(sizeOptions_.size()) &&
               selectedRow_ != currentSizeIndex()) {
      needsCustomPreview = SETTINGS.sdFontFamilyName[0] != '\0';
    }
    if (needsCustomPreview) queueBlockingFeedback(StrId::STR_LOADING_FONT_PREVIEW);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedRow_ < 0) {
      selectedTab_ = ButtonNavigator::nextIndex(selectedTab_, TabCount);
      selectedRow_ = -1;
      rebuildSettings();
      requestUpdate();
    } else {
      handleSelection();
    }
    return;
  }

  buttonNavigator_.onNextRelease([this] { moveSelection(1); });
  buttonNavigator_.onPreviousRelease([this] { moveSelection(-1); });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Right}, [this] { moveSelection(1); });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Left}, [this] { moveSelection(-1); });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Down}, [this] { moveTab(1); });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Up}, [this] { moveTab(-1); });
}

void TextSettingsActivity::invalidatePreviewLocked() {
  preparedPreviewFontId_ = 0;
  customPreviewFontIndex_ = -1;
  customPreviewFontSize_ = UINT8_MAX;
  customPreviewSnapshot_.reset();
  customPreviewSnapshotSize_ = 0;
  previewLines_.clear();
  customPreviewPending_ = SETTINGS.sdFontFamilyName[0] != '\0';
}

void TextSettingsActivity::refreshPreviewAfterSettingChange(const StrId settingId) {
  if (!settingAffectsTextPreview(settingId)) {
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    invalidatePreviewLocked();
  }
  if (SETTINGS.sdFontFamilyName[0] != '\0') showBlockingFeedback(StrId::STR_LOADING_FONT_PREVIEW);
  requestUpdate();
}

void TextSettingsActivity::applyFontSelection(const int index) {
  if (index < 0 || index >= static_cast<int>(fonts_.size())) return;
  const uint8_t preferredPoint =
      effectivePointSize(sdFontSystem.registry(), SETTINGS.sdFontFamilyName, SETTINGS.fontSize);
  bool applied = false;
  {
    RenderLock lock(*this);
    const auto& font = fonts_[index];
    if (font.isBuiltin) {
      SETTINGS.fontFamily = font.settingIndex;
      SETTINGS.fontSize = ReaderFontSize::closestIndex(preferredPoint, ReaderFontSize::BUILTIN_COUNT);
      SETTINGS.sdFontFamilyName[0] = '\0';
      applied = true;
    } else {
      const int sdIndex = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
      const auto& families = sdFontSystem.registry().getFamilies();
      if (sdIndex >= 0 && sdIndex < static_cast<int>(families.size())) {
        const int logicalSize = families[sdIndex].findClosestReaderSizeEnum(preferredPoint);
        if (logicalSize >= 0) {
          SETTINGS.fontSize = static_cast<uint8_t>(logicalSize);
          std::strncpy(SETTINGS.sdFontFamilyName, families[sdIndex].name.c_str(),
                       sizeof(SETTINGS.sdFontFamilyName) - 1);
          SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
          applied = true;
        }
      }
    }
    if (applied) {
      sdFontSystem.releaseLoadedFont(renderer);
      invalidatePreviewLocked();
    }
  }
  if (!applied) return;
  rebuildSizeOptions();
  persistSettings();
  requestUpdate();
}

void TextSettingsActivity::applySizeSelection(const int index) {
  if (index < 0 || index >= static_cast<int>(sizeOptions_.size())) return;
  {
    RenderLock lock(*this);
    SETTINGS.fontSize = sizeOptions_[index];
    sdFontSystem.releaseLoadedFont(renderer);
    invalidatePreviewLocked();
  }
  persistSettings();
  requestUpdate();
}

void TextSettingsActivity::handleSelection() {
  if (selectedRow_ < 0 || selectedRow_ >= currentListSize()) return;
  if (selectedTab_ == Font) {
    if (selectedRow_ != currentFontIndex()) applyFontSelection(selectedRow_);
    return;
  }
  if (selectedTab_ == Size) {
    if (selectedRow_ != currentSizeIndex()) applySizeSelection(selectedRow_);
    return;
  }

  const SettingInfo& setting = settings_[selectedRow_];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
    persistSettings();
    rebuildSettings();
    refreshPreviewAfterSettingChange(setting.nameId);
    return;
  }
  if (setting.type != SettingType::ENUM) return;

  const uint8_t current = setting.valueGetter ? setting.valueGetter() : SETTINGS.*(setting.valuePtr);
  auto select = [this, settingId = setting.nameId, valuePtr = setting.valuePtr,
                 setter = setting.valueSetter](const int index) {
    if (setter) {
      setter(static_cast<uint8_t>(index));
    } else if (valuePtr) {
      SETTINGS.*valuePtr = static_cast<uint8_t>(index);
    }
    persistSettings();
    rebuildSettings();
    refreshPreviewAfterSettingChange(settingId);
  };
  if (!setting.enumStringValues.empty()) {
    optionPopup_.show(setting.nameId, setting.enumStringValues, current, std::move(select));
  } else if (!setting.enumValues.empty()) {
    optionPopup_.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), current,
                      std::move(select));
  }
  requestUpdate();
}

int TextSettingsActivity::currentFontIndex() const {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    for (int index = CrossPointSettings::BUILTIN_FONT_COUNT; index < static_cast<int>(fonts_.size()); ++index) {
      if (fonts_[index].name == SETTINGS.sdFontFamilyName) return index;
    }
  }
  return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
}

int TextSettingsActivity::currentSizeIndex() const {
  const auto exact = std::find(sizeOptions_.begin(), sizeOptions_.end(), SETTINGS.fontSize);
  if (exact != sizeOptions_.end()) return static_cast<int>(std::distance(sizeOptions_.begin(), exact));

  const uint8_t wanted = ReaderFontSize::pointSize(SETTINGS.fontSize);
  uint8_t bestDelta = UINT8_MAX;
  int bestIndex = 0;
  for (int index = 0; index < static_cast<int>(sizeOptions_.size()); ++index) {
    const uint8_t candidate = ReaderFontSize::pointSize(sizeOptions_[index]);
    const uint8_t delta = candidate > wanted ? candidate - wanted : wanted - candidate;
    if (delta < bestDelta) {
      bestDelta = delta;
      bestIndex = index;
    }
  }
  return bestIndex;
}

std::string TextSettingsActivity::valueLabel(const int index) const {
  const SettingInfo& setting = settings_[index];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    return I18N.get((SETTINGS.*(setting.valuePtr)) ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM) {
    const uint8_t value = setting.valueGetter ? setting.valueGetter() : SETTINGS.*(setting.valuePtr);
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) return I18N.get(setting.enumValues[value]);
  }
  return {};
}

std::string TextSettingsActivity::sizeLabel(const int index) const {
  if (index < 0 || index >= static_cast<int>(sizeOptions_.size())) return {};
  const uint8_t logicalSize = sizeOptions_[index];
  const uint8_t actual = SETTINGS.sdFontFamilyName[0] == '\0'
                             ? ReaderFontSize::pointSize(logicalSize)
                             : sdFontSystem.selectedPointSize(SETTINGS.sdFontFamilyName, logicalSize);
  return std::to_string(actual ? actual : ReaderFontSize::pointSize(logicalSize)) + " pt";
}

std::string TextSettingsActivity::selectedFontName() const {
  if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
  return I18N.get(StrId::STR_NOTO_SERIF);
}

uint8_t TextSettingsActivity::selectedPointSize() const {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const uint8_t pointSize = sdFontSystem.selectedPointSize(SETTINGS.sdFontFamilyName, SETTINGS.fontSize);
    if (pointSize != 0) return pointSize;
  }
  return ReaderFontSize::pointSize(SETTINGS.fontSize);
}

void TextSettingsActivity::preparePreviewLines(const int fontId, const char* text, const int width,
                                               const int maxLines) {
  previewLines_.clear();
  if (fontId == 0 || !text || *text == '\0' || width <= 0 || maxLines <= 0) return;

  BlockStyle style;
  style.alignment = SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE
                        ? CssTextAlign::Justify
                        : static_cast<CssTextAlign>(SETTINGS.paragraphAlignment);
  if (SETTINGS.forceParagraphIndents) {
    style.textIndentDefined = true;
    style.textIndent = static_cast<int16_t>(renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 2);
  }

  ParsedText parsed(SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents, SETTINGS.hyphenationEnabled,
                    SETTINGS.focusReadingEnabled, SETTINGS.wordSpacing, style);
  const char* cursor = text;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
    const char* start = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') ++cursor;
    if (cursor != start) parsed.addWord(std::string(start, cursor - start), EpdFontFamily::REGULAR);
  }
  parsed.layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(std::min(width, static_cast<int>(UINT16_MAX))),
                               [this, maxLines](std::shared_ptr<TextBlock> line, uint32_t) {
                                 if (static_cast<int>(previewLines_.size()) < maxLines)
                                   previewLines_.push_back(std::move(line));
                               });
}

void TextSettingsActivity::drawPreparedPreview(const int fontId) const {
  int y = previewTextY_;
  for (int lineIndex = 0; lineIndex < static_cast<int>(previewLines_.size()); ++lineIndex) {
    if (y + previewLineHeight_ > previewTextBottom_) break;
    previewLines_[lineIndex]->render(renderer, fontId, previewTextX_, y);
    y += previewLineHeight_;
    if (SETTINGS.extraParagraphSpacing && lineIndex == 0 && previewLines_.size() > 1) {
      y += std::max(2, previewLineHeight_ / 3);
    }
  }
}

void TextSettingsActivity::renderPreviewPane(const int top, const int height, const int fontId, const char* fontName,
                                             const bool cachedCustomPreview, const bool prepareForAntiAliasing) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  if (width <= 0 || height <= 0) return;

  const int labelHeight = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelY = top + height - labelHeight;
  char previewLabel[128]{};
  std::snprintf(previewLabel, sizeof(previewLabel), tr(STR_TEXT_PREVIEW_FORMAT), fontName,
                static_cast<unsigned>(selectedPointSize()));
  renderer.drawText(UI_10_FONT_ID, left, labelY, previewLabel);
  if (fontId == 0 || (cachedCustomPreview && !prepareForAntiAliasing)) {
    previewLines_.clear();
    return;
  }

  if (preparedPreviewFontId_ != fontId) {
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->prewarmCache(fontId, tr(STR_FONT_PREVIEW_TEXT), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    preparedPreviewFontId_ = fontId;
  }
  const int previewBottom = labelY - 6;
  const int previewAreaHeight = std::max(0, previewBottom - top);
  const int horizontalMargin = std::clamp<int>(SETTINGS.screenMargin, 0, std::max(0, width / 4));
  const int textLeft = left + horizontalMargin;
  const int textWidth = std::max(1, width - horizontalMargin * 2);
  const bool darkMode = SETTINGS.readerDarkMode != 0;

  const int baseLineHeight = std::max(1, renderer.getLineHeight(fontId));
  const int lineHeight =
      std::max(renderer.getTextHeight(fontId), static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression()));
  const int maxLines = std::max(1, previewAreaHeight / std::max(1, lineHeight));
  previewTextX_ = textLeft;
  previewTextY_ = top;
  previewTextBottom_ = previewBottom;
  previewLineHeight_ = lineHeight;
  preparePreviewLines(fontId, tr(STR_FONT_PREVIEW_TEXT), textWidth, maxLines);
  if (!cachedCustomPreview) {
    drawPreparedPreview(fontId);
    if (darkMode && previewAreaHeight > 0) renderer.invertRect(left, top, width, previewAreaHeight);
  }
}

void TextSettingsActivity::render(RenderLock&&) {
  if (renderBlockingFeedbackOverlay()) return;
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TEXT_SETTINGS));

  const int previewTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewHeight = std::clamp(height / 6, 78, 132);
  const int previewLeft = metrics.contentSidePadding;
  const int previewWidth = width - metrics.contentSidePadding * 2;
  const int previewLabelHeight = renderer.getTextHeight(UI_10_FONT_ID);
  const int previewTextHeight = previewHeight - previewLabelHeight - 6;
  const bool customFont = SETTINGS.sdFontFamilyName[0] != '\0';
  const int fontIndex = currentFontIndex();
  const bool cachedCustomPreview = customFont && customPreviewSnapshot_ && customPreviewSnapshotSize_ > 0 &&
                                   customPreviewFontIndex_ == fontIndex && customPreviewFontSize_ == SETTINGS.fontSize;
  const bool antiAliasingSettingSelected = selectedTab_ == Style && selectedRow_ >= 0 &&
                                           selectedRow_ < static_cast<int>(settings_.size()) &&
                                           settings_[selectedRow_].nameId == StrId::STR_TEXT_AA;
  const bool showAntiAliasingPreview = antiAliasingSettingSelected && SETTINGS.textAntiAliasing &&
                                       !SETTINGS.readerDarkMode && renderer.supportsStripGrayscale();

  int previewFontId = customFont ? 0 : SETTINGS.getReaderFontId();
  bool loadedCustomPreview = false;
  bool releaseCustomAfterRender = false;
  size_t pendingSnapshotSize = 0;
  std::unique_ptr<uint8_t[]> pendingSnapshot;
  if (customFont && !cachedCustomPreview && customPreviewPending_) {
    // Reserve the small 1-bit preview snapshot before loading the much larger
    // .cpfont. This avoids asking a fragmented heap for another contiguous
    // allocation while the custom font is resident.
    pendingSnapshotSize = renderer.getRegionByteSize(previewLeft, previewTop, previewWidth, previewTextHeight);
    pendingSnapshot.reset(pendingSnapshotSize > 0 ? new (std::nothrow) uint8_t[pendingSnapshotSize] : nullptr);
    sdFontSystem.ensureLoaded(renderer, false);
    previewFontId = sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.fontSize);
    loadedCustomPreview = previewFontId != 0;
  } else if (customFont && !cachedCustomPreview) {
    // Snapshot allocation is expected to succeed. If it did not, keep using
    // the already resident font so the preview never degrades to a label-only
    // placeholder for the rest of this screen.
    previewFontId = sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.fontSize);
  } else if (customFont && showAntiAliasingPreview) {
    sdFontSystem.ensureLoaded(renderer, false);
    previewFontId = sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.fontSize);
    loadedCustomPreview = previewFontId != 0;
    releaseCustomAfterRender = loadedCustomPreview;
  }

  const std::string fontName = selectedFontName();
  renderPreviewPane(previewTop, previewHeight, previewFontId, fontName.c_str(), cachedCustomPreview,
                    showAntiAliasingPreview);
  if (cachedCustomPreview && !renderer.copyBufferToRegion(previewLeft, previewTop, previewWidth, previewTextHeight,
                                                          customPreviewSnapshot_.get(), customPreviewSnapshotSize_)) {
    customPreviewSnapshot_.reset();
    customPreviewSnapshotSize_ = 0;
    customPreviewFontIndex_ = -1;
    customPreviewFontSize_ = UINT8_MAX;
    customPreviewPending_ = true;
  }

  if (customFont && customPreviewPending_) {
    bool captured = false;
    if (loadedCustomPreview && pendingSnapshot &&
        renderer.copyRegionToBuffer(previewLeft, previewTop, previewWidth, previewTextHeight, pendingSnapshot.get(),
                                    pendingSnapshotSize)) {
      customPreviewSnapshot_ = std::move(pendingSnapshot);
      customPreviewSnapshotSize_ = pendingSnapshotSize;
      customPreviewFontIndex_ = fontIndex;
      customPreviewFontSize_ = SETTINGS.fontSize;
      captured = true;
    }
    customPreviewPending_ = false;
    preparedPreviewFontId_ = 0;
    releaseCustomAfterRender = captured || !loadedCustomPreview;
  }

  const int tabTop = previewTop + previewHeight + PREVIEW_TO_TABS_GAP;
  std::array<TabInfo, TabCount> tabs{};
  for (int tab = 0; tab < TabCount; ++tab) {
    tabs[static_cast<size_t>(tab)] = {I18N.get(TAB_LABELS[tab]), tab == selectedTab_};
  }
  GUI.drawTabBar(renderer, Rect{0, tabTop, width, metrics.tabBarHeight}, tabs, selectedRow_ < 0);

  const int listTop = tabTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight = height - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const Rect listRect{0, listTop, width, std::max(0, listHeight)};
  if (selectedTab_ == Font) {
    const int selected = currentFontIndex();
    GUI.drawList(
        renderer, listRect, static_cast<int>(fonts_.size()), selectedRow_,
        [this](const int index) { return fonts_[index].name; }, nullptr, nullptr,
        [selected](const int index) -> std::string { return index == selected ? tr(STR_SELECTED) : ""; }, true);
  } else if (selectedTab_ == Size) {
    const int selected = currentSizeIndex();
    GUI.drawList(
        renderer, listRect, static_cast<int>(sizeOptions_.size()), selectedRow_,
        [this](const int index) { return sizeLabel(index); }, nullptr, nullptr,
        [selected](const int index) -> std::string { return index == selected ? tr(STR_SELECTED) : ""; }, true);
  } else {
    GUI.drawList(
        renderer, listRect, static_cast<int>(settings_.size()), selectedRow_,
        [this](const int index) { return std::string(I18N.get(settings_[index].nameId)); }, nullptr, nullptr,
        [this](const int index) { return valueLabel(index); }, true);
  }

  const bool toggle =
      selectedTab_ >= Layout && selectedRow_ >= 0 && settings_[selectedRow_].type == SettingType::TOGGLE;
  const char* confirm = selectedRow_ < 0 ? I18N.get(TAB_LABELS[ButtonNavigator::nextIndex(selectedTab_, TabCount)])
                        : toggle         ? tr(STR_TOGGLE)
                                         : tr(STR_SELECT);
  const char* back = selectedRow_ < 0 ? tr(STR_BACK) : I18N.get(TAB_LABELS[selectedTab_]);
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  if (showAntiAliasingPreview && previewFontId != 0 && !previewLines_.empty()) {
    ReaderUtils::renderAntiAliased(renderer, [this, previewFontId] { drawPreparedPreview(previewFontId); });
  }
  if (releaseCustomAfterRender) sdFontSystem.releaseLoadedFont(renderer);
}
