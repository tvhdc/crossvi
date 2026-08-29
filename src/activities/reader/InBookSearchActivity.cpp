#include "InBookSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <new>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InBookSearchActivity::onEnter() {
  Activity::onEnter();
  normalized = makeBookSearchQuery(query);
  total = text ? text->getFileSize() : 0;
  buffer.reset(new (std::nothrow) uint8_t[CHUNK_BYTES]);
  running = text != nullptr && !normalized.empty() && buffer != nullptr &&
            Storage.openFileForRead("SEARCH", text->getPath(), contentFile);
  failed = !running;
  suppressInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  startOffset = std::min(startOffset, total);
  cursor = startOffset;
  reportedCursor = cursor;
  wrapped = false;
  hasLastMatch = false;
  overlap.clear();
  requestUpdate();
}

void InBookSearchActivity::onExit() {
  contentFile.close();
  Activity::onExit();
}

void InBookSearchActivity::finishWithOffset() {
  ActivityResult result;
  result.isCancelled = resultCount == 0;
  if (resultCount != 0) {
    ProgressChangeResult jump;
    jump.textByteOffset =
        static_cast<uint32_t>(std::min(results[selectedResult].offset, static_cast<size_t>(UINT32_MAX)));
    jump.hasTextByteOffset = true;
    result.data = std::move(jump);
  }
  setResult(std::move(result));
  finish();
}

void InBookSearchActivity::scanChunk() {
  if (!running || !text) return;
  const size_t limit = wrapped ? startOffset : total;
  if (cursor >= limit) {
    if (!wrapped && startOffset != 0) {
      cursor = 0;
      overlap.clear();
      wrapped = true;
      hasLastMatch = false;
    } else {
      running = false;
    }
    return;
  }
  const size_t length = std::min(CHUNK_BYTES, limit - cursor);
  if (!text->readContent(contentFile, buffer.get(), cursor, length)) {
    failed = true;
    running = false;
    return;
  }

  std::string sample;
  sample.reserve(overlap.size() + length);
  sample = overlap;
  sample.append(reinterpret_cast<const char*>(buffer.get()), length);

  // BookSearchUtils applies the same NFC/diacritic-folding rules as the
  // library search. Scan overlapping windows because its defensive candidate
  // limit is intentionally small and a match may cross a chunk boundary.
  constexpr size_t WINDOW = BOOK_SEARCH_CANDIDATE_BYTES;
  const size_t windowBase = chunkedSearchWindowBase(cursor, overlap.size());
  bool matched = false;
  for (size_t offset = 0; offset < sample.size() && !matched; offset += WINDOW / 2) {
    const std::string_view window = std::string_view(sample).substr(offset, std::min(WINDOW, sample.size() - offset));
    if (matchBookSearch(normalized, window) != BookSearchMatch::None) {
      const size_t absoluteOffset = windowBase + offset;
      const bool duplicate =
          hasLastMatch && bookSearchRangesOverlap(lastMatchedOffset, lastMatchedBytes, absoluteOffset, window.size());
      if (duplicate) continue;
      Result& result = results[resultCount++];
      result.offset = absoluteOffset;
      result.snippetBytes = std::min(window.size(), result.snippet.size());
      std::copy_n(window.data(), result.snippetBytes, result.snippet.data());
      lastMatchedOffset = result.offset;
      lastMatchedBytes = result.snippetBytes;
      hasLastMatch = true;
      matched = true;
    }
  }

  const size_t keep = std::min(OVERLAP_BYTES, sample.size());
  overlap.assign(sample.data() + sample.size() - keep, keep);
  cursor += length;
  if (resultCount == results.size()) running = false;
}

void InBookSearchActivity::loop() {
  if (suppressInitialConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressInitialConfirmRelease = false;
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }
  if (running) {
    scanChunk();
    // Keep input cooperative without forcing an e-ink refresh after every
    // small SD read. A terminal state is still painted immediately.
    if (!running || failed || cursor < reportedCursor || cursor - reportedCursor >= PROGRESS_UPDATE_BYTES) {
      reportedCursor = cursor;
      requestUpdate();
    }
    return;
  }
  if (resultCount != 0) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedResult = selectedResult == 0 ? resultCount - 1 : selectedResult - 1;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedResult = (selectedResult + 1) % resultCount;
      requestUpdate();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finishWithOffset();
  }
}

void InBookSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_SEARCH_IN_BOOK));

  const Rect listRect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, width,
                      std::max(1, height - metrics.topPadding - metrics.headerHeight - metrics.buttonHintsHeight -
                                      metrics.verticalSpacing)};
  if (running) {
    GUI.drawList(renderer, listRect, 1, 0, [](int) { return std::string(tr(STR_LOADING)); });
  } else if (resultCount != 0) {
    GUI.drawList(renderer, listRect, resultCount, selectedResult, [this](int index) {
      const Result& result = results[static_cast<size_t>(index)];
      return std::string(result.snippet.data(), result.snippetBytes);
    });
  } else {
    const StrId status = failed ? StrId::STR_ERROR_GENERAL_FAILURE : StrId::STR_NO_SEARCH_RESULTS;
    GUI.drawList(renderer, listRect, 1, 0, [status](int) { return std::string(I18N.get(status)); });
  }
  if (running && total > 0) {
    GUI.drawProgressBar(renderer, Rect{40, height / 2 + 24, width - 80, 18}, std::min(cursor, total), total);
  }
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), resultCount != 0 ? tr(STR_SELECT) : "",
                            resultCount != 0 ? tr(STR_DIR_UP) : "", resultCount != 0 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
