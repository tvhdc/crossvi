#pragma once

#include <Epub.h>
#include <Epub/EpubRenderMode.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/BookSearchUtils.h"

class Section;

// Searches rendered EPUB pages one at a time. A Section is built incrementally
// when its matching cache is missing, so search never materializes a complete
// spine HTML document in RAM.
class EpubInBookSearchActivity final : public Activity {
 public:
  struct Layout {
    int fontId = 0;
    float lineCompression = 1.0f;
    bool extraParagraphSpacing = false;
    uint8_t paragraphAlignment = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    uint8_t imageRendering = 0;
    bool focusReadingEnabled = false;
    uint8_t wordSpacing = 0;
    EpubRenderMode renderMode = EpubRenderMode::Full;
    bool forceParagraphIndents = false;
  };

  EpubInBookSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                           std::string query, int startSpine, int startPage, const Layout& layout);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return searching_; }

 private:
  struct Result {
    int spine = 0;
    int page = 0;
    std::string snippet;
  };

  static constexpr size_t MAX_RESULTS = 32;
  static constexpr size_t MAX_PAGE_TEXT_BYTES = 4096;

  std::shared_ptr<Epub> epub_;
  std::string query_;
  BookSearchQuery normalized_;
  Layout layout_;
  std::unique_ptr<Section> section_;
  std::vector<Result> results_;
  int startSpine_ = 0;
  int startPage_ = 0;
  int spine_ = 0;
  int page_ = 0;
  int selected_ = 0;
  uint8_t pagesSinceUpdate_ = 0;
  bool wrapped_ = false;
  bool searching_ = false;
  bool failed_ = false;

  bool openSection();
  bool scanCurrentPage();
  void advancePage();
  void finishWithSelection();
};
