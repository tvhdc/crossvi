#include "EpubInBookSearchActivity.h"

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/EpubSearchTraversal.h"

EpubInBookSearchActivity::EpubInBookSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::shared_ptr<Epub> epub, std::string query, const int startSpine,
                                                   const int startPage, const Layout& layout)
    : Activity("EpubInBookSearch", renderer, mappedInput),
      epub_(std::move(epub)),
      query_(std::move(query)),
      layout_(layout),
      startSpine_(std::max(0, startSpine)),
      startPage_(std::max(0, startPage)),
      spine_(startSpine_),
      page_(startPage_) {}

void EpubInBookSearchActivity::onEnter() {
  Activity::onEnter();
  // Missing section caches must be laid out with the same font as the reader.
  // Keep that font only for the bounded search activity, then release it.
  sdFontSystem.ensureLoaded(renderer, false);
  layout_.fontId = SETTINGS.getReaderFontId();
  normalized_ = makeBookSearchQuery(query_);
  if (!epub_ || normalized_.empty() || epub_->getSpineItemsCount() <= 0 || layout_.viewportWidth == 0 ||
      layout_.viewportHeight == 0) {
    failed_ = true;
  } else {
    startSpine_ = std::min(startSpine_, epub_->getSpineItemsCount() - 1);
    spine_ = startSpine_;
    searching_ = true;
  }
  requestUpdate();
}

void EpubInBookSearchActivity::onExit() {
  section_.reset();
  sdFontSystem.releaseLoadedFont(renderer);
  Activity::onExit();
}

bool EpubInBookSearchActivity::openSection() {
  section_ = std::make_unique<Section>(epub_, spine_, renderer);
  const bool loaded = section_->loadSectionFile(
      layout_.fontId, layout_.lineCompression, layout_.extraParagraphSpacing, layout_.paragraphAlignment,
      layout_.viewportWidth, layout_.viewportHeight, layout_.hyphenationEnabled, layout_.embeddedStyle,
      layout_.imageRendering, layout_.focusReadingEnabled, layout_.wordSpacing, layout_.renderMode,
      layout_.forceParagraphIndents);
  if (!EpubSearchTraversal::needsBuild(loaded, loaded && section_->isPartial())) {
    if (!wrapped_ && spine_ == startSpine_) {
      startPage_ = EpubSearchTraversal::clampPage(startPage_, section_->pageCount);
      page_ = startPage_;
    }
    return true;
  }
  return section_->startBuild(layout_.fontId, layout_.lineCompression, layout_.extraParagraphSpacing,
                              layout_.paragraphAlignment, layout_.viewportWidth, layout_.viewportHeight,
                              layout_.hyphenationEnabled, layout_.embeddedStyle, layout_.imageRendering,
                              layout_.focusReadingEnabled, layout_.wordSpacing, layout_.renderMode,
                              layout_.forceParagraphIndents);
}

bool EpubInBookSearchActivity::scanCurrentPage() {
  if (!section_) return false;
  std::unique_ptr<Page> page = section_->loadPage(page_);
  if (!page) return false;
  std::string text;
  text.reserve(512);
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    for (uint16_t word = 0; word < block->wordCount(); ++word) {
      const uint16_t bytes = block->wordTextLen(word);
      if (bytes == 0 || text.size() + bytes + 1 > MAX_PAGE_TEXT_BYTES) break;
      if (!text.empty()) text.push_back(' ');
      text.append(block->wordText(word), bytes);
    }
  }
  constexpr size_t window = BOOK_SEARCH_CANDIDATE_BYTES;
  const std::string_view textView(text);
  for (size_t offset = 0; offset < textView.size(); offset += window / 2) {
    const std::string_view candidate = textView.substr(offset, std::min(window, textView.size() - offset));
    if (matchBookSearch(normalized_, candidate) == BookSearchMatch::None) continue;
    Result result;
    result.spine = spine_;
    result.page = page_;
    result.snippet.assign(candidate.data(), std::min<size_t>(candidate.size(), 160));
    results_.push_back(std::move(result));
    break;
  }
  return true;
}

void EpubInBookSearchActivity::advancePage() {
  ++page_;
  if (section_ && page_ < static_cast<int>(section_->pageCount)) return;
  if (section_ && section_->isBuilding()) return;
  section_.reset();
  ++spine_;
  if (spine_ >= epub_->getSpineItemsCount()) {
    spine_ = 0;
    wrapped_ = true;
  }
  page_ = 0;
}

void EpubInBookSearchActivity::finishWithSelection() {
  ActivityResult result;
  result.isCancelled = results_.empty();
  if (!results_.empty()) {
    ProgressChangeResult jump;
    jump.spineIndex = results_[selected_].spine;
    jump.page = results_[selected_].page;
    jump.hasSavedProgress = true;
    result.data = std::move(jump);
  }
  setResult(std::move(result));
  finish();
}

void EpubInBookSearchActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (!searching_) {
    if (!results_.empty()) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        selected_ = selected_ == 0 ? static_cast<int>(results_.size()) - 1 : selected_ - 1;
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        selected_ = (selected_ + 1) % static_cast<int>(results_.size());
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) finishWithSelection();
    return;
  }
  if (!section_ && !openSection()) {
    searching_ = false;
    failed_ = true;
    requestUpdate();
    return;
  }
  if (section_->isBuilding() && !section_->buildSomeMore(1)) {
    searching_ = false;
    failed_ = true;
    requestUpdate();
    return;
  }
  if (!section_->isBuilding() && !wrapped_ && spine_ == startSpine_ && section_->pageCount > 0 &&
      startPage_ >= section_->pageCount) {
    startPage_ = EpubSearchTraversal::clampPage(startPage_, section_->pageCount);
    page_ = startPage_;
  }
  if (wrapped_ && spine_ == startSpine_ && page_ >= startPage_) {
    searching_ = false;
    selected_ = 0;
    requestUpdate();
    return;
  }
  if (page_ < static_cast<int>(section_->pageCount)) {
    if (!scanCurrentPage()) {
      searching_ = false;
      failed_ = true;
    } else if (results_.size() >= MAX_RESULTS) {
      searching_ = false;
    } else {
      advancePage();
    }
    if (!searching_) requestUpdate();
  } else if (!section_->isBuilding()) {
    advancePage();
  }
}

void EpubInBookSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_SEARCH_IN_BOOK));
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (searching_) {
    GUI.drawList(renderer, Rect{0, top, width, std::max(1, bottom - top)}, 1, 0,
                 [](int) { return std::string(tr(STR_LOADING)); });
  } else if (!results_.empty()) {
    GUI.drawList(renderer, Rect{0, top, width, std::max(1, bottom - top)}, results_.size(), selected_,
                 [this](int index) { return results_[static_cast<size_t>(index)].snippet; });
  } else {
    const StrId message = failed_ ? StrId::STR_ERROR_GENERAL_FAILURE : StrId::STR_NO_SEARCH_RESULTS;
    GUI.drawList(renderer, Rect{0, top, width, std::max(1, bottom - top)}, 1, 0,
                 [message](int) { return std::string(I18N.get(message)); });
  }
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), results_.empty() ? "" : tr(STR_SELECT),
                            results_.empty() ? "" : tr(STR_DIR_UP), results_.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
