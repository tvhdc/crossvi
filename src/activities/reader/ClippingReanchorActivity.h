#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "clippings/ClippingPageTools.h"
#include "clippings/ClippingStore.h"

class Section;

// Explicit, bounded EPUB re-anchor flow. The parent reader is paused while
// this activity reads at most nine already-laid-out pages, one per loop tick.
class ClippingReanchorActivity final : public Activity {
 public:
  ClippingReanchorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Section& section,
                           ClippingStore& store, size_t clippingIndex, uint16_t firstPage, uint16_t lastPage,
                           uint32_t layoutFingerprint, int fontId, int marginLeft, int marginTop);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void cancel();
  void complete();
  void fail();

  Section& section_;
  ClippingStore& store_;
  size_t clippingIndex_ = 0;
  uint16_t firstPage_ = 0;
  uint16_t lastPage_ = 0;
  uint16_t nextPage_ = 0;
  uint16_t pagesScanned_ = 0;
  uint32_t layoutFingerprint_ = 0;
  int fontId_ = 0;
  int marginLeft_ = 0;
  int marginTop_ = 0;
  std::unique_ptr<ClippingPageTools::ExactReanchorMatcher> matcher_;
  std::atomic<bool> firstFrameRendered_{false};
  bool finished_ = false;
};
