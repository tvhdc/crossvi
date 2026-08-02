#include "ClippingReanchorActivity.h"

#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"

ClippingReanchorActivity::ClippingReanchorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   Section& section, ClippingStore& store, const size_t clippingIndex,
                                                   const uint16_t firstPage, const uint16_t lastPage,
                                                   const uint32_t layoutFingerprint, const int fontId,
                                                   const int marginLeft, const int marginTop)
    : Activity("ClippingReanchor", renderer, mappedInput),
      section_(section),
      store_(store),
      clippingIndex_(clippingIndex),
      firstPage_(firstPage),
      lastPage_(lastPage),
      nextPage_(firstPage),
      layoutFingerprint_(layoutFingerprint),
      fontId_(fontId),
      marginLeft_(marginLeft),
      marginTop_(marginTop) {}

void ClippingReanchorActivity::onEnter() {
  Activity::onEnter();
  const auto* clipping = store_.at(clippingIndex_);
  std::string text;
  if (!clipping || firstPage_ > lastPage_ ||
      static_cast<size_t>(lastPage_ - firstPage_) + 1 > ClippingPageTools::MAX_REANCHOR_PAGES ||
      !store_.readText(clippingIndex_, text)) {
    fail();
    return;
  }
  matcher_ =
      std::make_unique<ClippingPageTools::ExactReanchorMatcher>(text, static_cast<size_t>(lastPage_ - firstPage_) + 1);
  requestUpdate();
}

void ClippingReanchorActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finished_ = true;
  finish();
}

void ClippingReanchorActivity::fail() {
  if (finished_) return;
  setResult(ActivityResult{});
  finished_ = true;
  finish();
}

void ClippingReanchorActivity::complete() {
  if (!matcher_) return fail();
  const ClippingPageTools::ReanchorResult result = matcher_->finish();
  if (result.status != ClippingPageTools::ReanchorStatus::Found) {
    LOG_ERR("CLIP", "Reanchor failed status=%u pages=%u range=%u-%u", static_cast<unsigned>(result.status),
            static_cast<unsigned>(pagesScanned_), static_cast<unsigned>(firstPage_), static_cast<unsigned>(lastPage_));
    return fail();
  }

  const auto* current = store_.at(clippingIndex_);
  if (!current) return fail();
  ClippingCodec::ClippingMetadata updated = *current;
  updated.startPage = result.startPage;
  updated.endPage = result.endPage;
  updated.pageCount = std::max<uint16_t>(section_.estimatedTotalPages(), static_cast<uint16_t>(result.endPage + 1));
  updated.startWordIndex = result.startWordIndex;
  updated.endWordIndex = result.endWordIndex;
  updated.wordCount = result.wordCount;
  updated.paragraphIndex = section_.getParagraphIndexForPage(result.startPage).value_or(UINT16_MAX);
  updated.pageFingerprint = result.startPageFingerprint;
  updated.layoutFingerprint = layoutFingerprint_;
  if (store_.updateLocation(clippingIndex_, updated) != ClippingStore::UpdateResult::Updated) return fail();

  setResult(PageResult{result.startPage});
  finished_ = true;
  finish();
}

void ClippingReanchorActivity::loop() {
  if (finished_) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (!firstFrameRendered_.load(std::memory_order_acquire) || !matcher_) return;
  if (nextPage_ > lastPage_) {
    complete();
    return;
  }

  RenderLock lock(*this);
  std::unique_ptr<Page> page = section_.loadPage(nextPage_);
  if (!page) {
    lock.unlock();
    fail();
    return;
  }
  const uint32_t pageFingerprint = ClippingPageTools::fingerprint(*page, renderer, fontId_, marginLeft_, marginTop_);
  const ClippingPageTools::ReanchorStatus status =
      matcher_->feedPage(nextPage_, pageFingerprint, *page, renderer, fontId_);
  ++nextPage_;
  ++pagesScanned_;
  lock.unlock();

  if (status == ClippingPageTools::ReanchorStatus::Ambiguous ||
      status == ClippingPageTools::ReanchorStatus::InvalidInput ||
      status == ClippingPageTools::ReanchorStatus::LimitExceeded) {
    fail();
    return;
  }
  if (nextPage_ > lastPage_) {
    complete();
  } else if (pagesScanned_ % 3 == 0) {
    requestUpdate();
  }
}

void ClippingReanchorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKMARKS_AND_HIGHLIGHTS));

  char progress[48]{};
  const unsigned total = static_cast<unsigned>(lastPage_ - firstPage_ + 1);
  std::snprintf(progress, sizeof(progress), "%s %u/%u", tr(STR_REANCHOR_PROGRESS), static_cast<unsigned>(pagesScanned_),
                total);
  const int contentY = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawHelpText(renderer, Rect{screen.x, contentY, screen.width, metrics.listRowHeight}, progress);

  const auto labels = mappedInput.mapLabels(tr(STR_REANCHOR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  firstFrameRendered_.store(true, std::memory_order_release);
}
