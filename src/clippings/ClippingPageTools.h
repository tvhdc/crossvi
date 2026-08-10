#pragma once

#include <Epub/Page.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ClippingCodec.h"

class GfxRenderer;

namespace ClippingPageTools {

struct SelectionPageAdvanceState {
  bool loaderAvailable = false;
  bool extractionComplete = false;
  bool selectionStarted = false;
  size_t wordCount = 0;
  int cursorOrder = -1;
  uint16_t currentPage = 0;
  uint16_t pageCount = 0;
  bool selectionFits = false;
};

inline bool canAdvanceSelectionPage(const SelectionPageAdvanceState& state) {
  return state.loaderAvailable && state.extractionComplete && state.selectionStarted && state.wordCount > 0 &&
         state.cursorOrder == static_cast<int>(state.wordCount - 1) && state.pageCount > 0 &&
         state.currentPage < static_cast<uint16_t>(state.pageCount - 1) && state.selectionFits;
}

// The caller supplies unique token indices captured from one rendered page.
// A selection reaches the real end only when it contains every token from its
// first selected token through the page's final textual token.
inline bool isContiguousTail(const uint16_t* indices, const size_t count, const uint32_t pageWordCount) {
  if (!indices || count == 0 || pageWordCount == 0) return false;
  uint16_t minimum = indices[0];
  uint16_t maximum = indices[0];
  for (size_t index = 1; index < count; ++index) {
    minimum = std::min(minimum, indices[index]);
    maximum = std::max(maximum, indices[index]);
  }
  return static_cast<uint32_t>(maximum) + 1 == pageWordCount && static_cast<uint32_t>(maximum - minimum) + 1 == count;
}

// Matches the selection activity's definition of a visible token, including
// common Unicode spacing characters emitted by EPUB layout.
bool hasVisibleText(std::string_view text);

// Stable identity for the complete rendered page, not a text-search key. It
// includes the viewport, font identity/metrics, offsets, and every available
// element/line geometry value, so a clipping is highlighted only on the exact
// layout from which it was created. Zero is never returned.
uint32_t fingerprint(const Page& page, const GfxRenderer& renderer, int fontId, int marginLeft, int marginTop);

struct LayoutIdentity {
  int32_t fontId = 0;
  float lineCompression = 1.0F;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  uint8_t wordSpacing = 0;
  uint8_t renderMode = 0;
  bool forceParagraphIndents = false;
};

// Identity of the exact Section pagination inputs. It deliberately excludes
// page content and page count, so one value applies to every page in a layout.
// Zero is never returned.
uint32_t layoutFingerprint(const LayoutIdentity& identity);

struct HighlightLine {
  int16_t left;
  int16_t right;
  int16_t top;
  int16_t bottom;
  int16_t y;
  bool backgroundSafe;
};

struct HighlightPlan {
  static constexpr size_t MAX_LINES = 192;
  // Only entries below count are initialized and read. Avoid clearing the
  // complete fixed-capacity backing store on every rendered page.
  std::array<HighlightLine, MAX_LINES> lines;
  size_t count = 0;
  bool truncated = false;

  HighlightPlan() noexcept {}

  // Invert after page text is rendered: light pages become black with white
  // text, while the reader's final dark-mode inversion produces the opposite.
  // Grayscale passes clear these bands so the crisp B/W highlight survives.
  void drawInverse(GfxRenderer& renderer) const;
  void clearGrayscale(GfxRenderer& renderer) const;
  void drawUnderline(GfxRenderer& renderer, bool fallbackOnly = false) const;
  // Compatibility path for existing readers until they adopt the two-pass
  // background API.
  void draw(GfxRenderer& renderer) const { drawUnderline(renderer); }
};

// Render-task-only, fixed-memory history for the user-facing truncation
// warning. It suppresses repeated popups for recently visited page layouts
// without allocating a set that can grow with the book.
class HighlightNoticeTracker {
 public:
  static constexpr size_t MAX_TRACKED_PAGES = 8;

  bool markIfNew(uint16_t spineIndex, uint16_t pageIndex, uint32_t pageFingerprint) {
    if (pageFingerprint == 0) return false;
    const PageIdentity candidate{spineIndex, pageIndex, pageFingerprint};
    for (size_t i = 0; i < count_; ++i) {
      if (pages_[i] == candidate) return false;
    }
    pages_[next_] = candidate;
    next_ = (next_ + 1) % pages_.size();
    if (count_ < pages_.size()) ++count_;
    return true;
  }

 private:
  struct PageIdentity {
    uint16_t spineIndex = 0;
    uint16_t pageIndex = 0;
    uint32_t pageFingerprint = 0;

    bool operator==(const PageIdentity&) const = default;
  };

  std::array<PageIdentity, MAX_TRACKED_PAGES> pages_{};
  size_t count_ = 0;
  size_t next_ = 0;
};

// Builds at most maxHighlights exact-page selections into fixed geometry once;
// grayscale strip passes can replay it without rescanning words or SD I/O.
HighlightPlan buildExactHighlightPlan(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft,
                                      int marginTop, const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                      uint16_t spineIndex, uint16_t pageIndex, uint32_t pageFingerprint,
                                      size_t maxHighlights = ClippingCodec::MAX_CLIPPINGS_PER_BOOK);

// Builds exact geometry for single- or multi-page EPUB highlights. Version 4
// entries use the shared layout fingerprint; migrated v3 entries (zero layout
// fingerprint) remain restricted to their exact start-page fingerprint.
HighlightPlan buildHighlightPlan(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                                 const std::vector<ClippingCodec::ClippingMetadata>& clippings, uint16_t spineIndex,
                                 uint16_t pageIndex, uint32_t pageFingerprint, uint32_t currentLayoutFingerprint,
                                 size_t maxHighlights = ClippingCodec::MAX_CLIPPINGS_PER_BOOK);

struct SourceWordAnchor {
  uint32_t start = 0;
  uint32_t end = 0;
};

// Matches the visible Page words to caller-supplied raw TXT byte ranges. The
// function performs no storage access and returns an empty plan when geometry
// and anchor counts disagree rather than painting an approximate range.
HighlightPlan buildTextAnchorHighlightPlan(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft,
                                           int marginTop, const SourceWordAnchor* anchors, size_t anchorCount,
                                           const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                           size_t maxHighlights = ClippingCodec::MAX_CLIPPINGS_PER_BOOK);

inline constexpr size_t MAX_REANCHOR_PAGES = 9;

enum class ReanchorStatus : uint8_t {
  Ready,
  Found,
  NotFound,
  Ambiguous,
  Cancelled,
  InvalidInput,
  LimitExceeded,
};

struct ReanchorResult {
  ReanchorStatus status = ReanchorStatus::NotFound;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint32_t startPageFingerprint = 0;
};

struct Cancellation {
  void* context = nullptr;
  bool (*requested)(void* context) = nullptr;
};

std::string normalizeReanchorText(std::string_view text);

// Incremental bounded exact matcher. It retains only the normalized target,
// a KMP prefix table, and a target-sized position ring; fed pages are never
// retained. Call finish() only after all candidate pages have been supplied.
class ExactReanchorMatcher {
 public:
  ExactReanchorMatcher(std::string_view target, size_t maxPages, Cancellation cancellation = {});

  ReanchorStatus feedPage(uint16_t pageIndex, uint32_t pageFingerprint, const Page& page, const GfxRenderer& renderer,
                          int fontId);
  ReanchorResult finish() const;

 private:
  struct Position {
    uint16_t page = 0;
    uint16_t word = 0;
    uint32_t ordinal = 0;
    uint32_t pageFingerprint = 0;
    bool wordStart = false;
    bool wordEnd = false;
  };

  static constexpr size_t MAX_TARGET_BYTES = ClippingCodec::MAX_TEXT_BYTES;
  std::string target_;
  std::array<uint16_t, MAX_TARGET_BYTES> prefix_{};
  std::array<Position, MAX_TARGET_BYTES> positions_{};
  std::string previousWord_;
  Cancellation cancellation_{};
  ReanchorResult match_{};
  ReanchorStatus status_ = ReanchorStatus::Ready;
  size_t maxPages_ = 0;
  size_t pagesFed_ = 0;
  size_t matchedBytes_ = 0;
  uint64_t emittedBytes_ = 0;
  uint32_t wordOrdinal_ = 0;
  int previousPage_ = -1;
  int previousLineY_ = 0;
  int previousEndX_ = 0;
  size_t matchCount_ = 0;

  bool cancelled() const;
  void feedByte(char byte, Position position);
  bool feedWord(std::string_view word, const Position& position, int lineY, int x, int width);
};

}  // namespace ClippingPageTools
