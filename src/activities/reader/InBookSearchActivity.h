#pragma once

#include <Txt.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/BookSearchUtils.h"

// Bounded, cooperative search for plain-text/Markdown readers.  The reader
// remains alive underneath this activity; only a small chunk and an overlap
// are held in RAM, so a large file cannot block the input loop for its full
// duration.
class InBookSearchActivity final : public Activity {
 public:
  InBookSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Txt* text, std::string query,
                       size_t startOffset)
      : Activity("InBookSearch", renderer, mappedInput),
        text(text),
        query(std::move(query)),
        startOffset(startOffset) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return running; }

 private:
  static constexpr size_t CHUNK_BYTES = 2048;
  static constexpr size_t OVERLAP_BYTES = 256;
  static constexpr size_t PROGRESS_UPDATE_BYTES = CHUNK_BYTES * 8;
  static constexpr size_t MAX_RESULTS = 32;
  static constexpr size_t MAX_SNIPPET_BYTES = 160;

  struct Result {
    size_t offset = 0;
    size_t snippetBytes = 0;
    std::array<char, MAX_SNIPPET_BYTES> snippet{};
  };

  Txt* text = nullptr;
  std::string query;
  BookSearchQuery normalized;
  std::string overlap;
  std::unique_ptr<uint8_t[]> buffer;
  HalFile contentFile;
  size_t cursor = 0;
  size_t total = 0;
  size_t startOffset = 0;
  size_t reportedCursor = 0;
  std::array<Result, MAX_RESULTS> results{};
  size_t resultCount = 0;
  size_t selectedResult = 0;
  size_t lastMatchedOffset = 0;
  size_t lastMatchedBytes = 0;
  bool running = false;
  bool failed = false;
  bool wrapped = false;
  bool hasLastMatch = false;
  bool suppressInitialConfirmRelease = false;

  void scanChunk();
  void finishWithOffset();
};
