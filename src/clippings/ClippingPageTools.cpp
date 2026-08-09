#include "ClippingPageTools.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace ClippingPageTools {
namespace {

size_t whitespaceBytesAt(const std::string_view text, const size_t offset) {
  const uint8_t first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) return std::isspace(first) ? 1 : 0;
  if (first == 0xC2 && offset + 1 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0xA0) return 2;
  if (first == 0xE2 && offset + 2 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0x80) {
    const uint8_t last = static_cast<uint8_t>(text[offset + 2]);
    if ((last >= 0x80 && last <= 0x8A) || last == 0xAF) return 3;
  }
  return 0;
}

bool startsParagraph(const std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    const size_t bytes = whitespaceBytesAt(text, offset);
    if (bytes == 0) return false;
    if (bytes == 3 && static_cast<uint8_t>(text[offset]) == 0xE2 && static_cast<uint8_t>(text[offset + 1]) == 0x80 &&
        static_cast<uint8_t>(text[offset + 2]) == 0x83) {
      return true;
    }
    offset += bytes;
  }
  return false;
}

bool startsWithAny(const std::string_view text, const std::initializer_list<std::string_view> values) {
  return std::any_of(values.begin(), values.end(), [&](const std::string_view value) {
    return text.size() >= value.size() && text.substr(0, value.size()) == value;
  });
}

bool endsWithAny(const std::string_view text, const std::initializer_list<std::string_view> values) {
  return std::any_of(values.begin(), values.end(), [&](const std::string_view value) {
    return text.size() >= value.size() && text.substr(text.size() - value.size()) == value;
  });
}

bool isClosingPunctuation(const std::string_view text) {
  return startsWithAny(text,
                       {",", ".", ";", ":", "!", "?", "%", ")", "]", "}", "'", "\"", "’", "”", "»", "…", "、", "。"});
}

bool isOpeningPunctuation(const std::string_view text) {
  return endsWithAny(text, {"(", "[", "{", "'", "\"", "‘", "“", "«"});
}

bool wordLikeBoundary(const std::string_view text, const bool front) {
  if (text.empty()) return false;
  const uint8_t byte = static_cast<uint8_t>(front ? text.front() : text.back());
  return byte >= 0x80 || std::isalnum(byte);
}

void addU16(uint32_t& checksum, const uint16_t value) {
  const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  checksum = ClippingCodec::crc32(bytes.data(), bytes.size(), checksum);
}

void addU32(uint32_t& checksum, const uint32_t value) {
  const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                                     static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  checksum = ClippingCodec::crc32(bytes.data(), bytes.size(), checksum);
}

void addU8(uint32_t& checksum, const uint8_t value) { checksum = ClippingCodec::crc32(&value, 1, checksum); }

void addBlockStyle(uint32_t& checksum, const BlockStyle& style) {
  addU16(checksum, static_cast<uint16_t>(style.alignment));
  addU16(checksum, static_cast<uint16_t>(style.marginTop));
  addU16(checksum, static_cast<uint16_t>(style.marginBottom));
  addU16(checksum, static_cast<uint16_t>(style.marginLeft));
  addU16(checksum, static_cast<uint16_t>(style.marginRight));
  addU16(checksum, static_cast<uint16_t>(style.paddingTop));
  addU16(checksum, static_cast<uint16_t>(style.paddingBottom));
  addU16(checksum, static_cast<uint16_t>(style.paddingLeft));
  addU16(checksum, static_cast<uint16_t>(style.paddingRight));
  addU16(checksum, static_cast<uint16_t>(style.textIndent));
  uint16_t flags = 0;
  flags |= style.textIndentDefined ? 1U << 0 : 0;
  flags |= style.textAlignDefined ? 1U << 1 : 0;
  flags |= style.isRtl ? 1U << 2 : 0;
  flags |= style.directionDefined ? 1U << 3 : 0;
  flags |= style.fromBrElement ? 1U << 4 : 0;
  addU16(checksum, flags);
}

uint32_t fingerprintImpl(const Page& page, const int fontId, const int viewportWidth, const int viewportHeight,
                         const int lineHeight, const int marginLeft, const int marginTop) {
  uint32_t checksum = 0;
  addU32(checksum, static_cast<uint32_t>(fontId));
  addU32(checksum, static_cast<uint32_t>(viewportWidth));
  addU32(checksum, static_cast<uint32_t>(viewportHeight));
  addU32(checksum, static_cast<uint32_t>(lineHeight));
  addU32(checksum, static_cast<uint32_t>(marginLeft));
  addU32(checksum, static_cast<uint32_t>(marginTop));
  addU32(checksum, static_cast<uint32_t>(page.elements.size()));

  for (const auto& element : page.elements) {
    if (!element) {
      addU16(checksum, 0);
      continue;
    }
    addU16(checksum, static_cast<uint16_t>(element->getTag()));
    addU16(checksum, static_cast<uint16_t>(element->xPos));
    addU16(checksum, static_cast<uint16_t>(element->yPos));
    if (element->getTag() == TAG_PageImage) {
      const auto& image = static_cast<const PageImage&>(*element).getImageBlock();
      addU16(checksum, static_cast<uint16_t>(image.getWidth()));
      addU16(checksum, static_cast<uint16_t>(image.getHeight()));
      continue;
    }
    if (element->getTag() != TAG_PageLine) continue;

    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) {
      addU16(checksum, 0);
      continue;
    }

    addU16(checksum, block->wordCount());
    addBlockStyle(checksum, block->getBlockStyle());
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const uint16_t length = block->wordTextLen(i);
      addU16(checksum, static_cast<uint16_t>(block->wordXpos(i)));
      addU16(checksum, static_cast<uint16_t>(block->wordStyle(i)));
      addU16(checksum, block->focusBoundary(i));
      addU16(checksum, block->focusSuffixX(i));
      addU16(checksum, length);
      checksum = ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(block->wordText(i)), length, checksum);
    }
  }
  return checksum == 0 ? 1 : checksum;
}

struct WordRange {
  uint32_t first = 0;
  uint32_t last = 0;
};

bool addHighlightLine(HighlightPlan& plan, HighlightLine candidate) {
  for (size_t index = 0; index < plan.count;) {
    HighlightLine& existing = plan.lines[index];
    const bool sameBand = existing.top == candidate.top && existing.bottom == candidate.bottom &&
                          existing.backgroundSafe == candidate.backgroundSafe;
    const bool overlaps = candidate.left <= static_cast<int>(existing.right) + 1 &&
                          existing.left <= static_cast<int>(candidate.right) + 1;
    if (!sameBand || !overlaps) {
      ++index;
      continue;
    }
    candidate.left = std::min(candidate.left, existing.left);
    candidate.right = std::max(candidate.right, existing.right);
    candidate.y = std::max(candidate.y, existing.y);
    plan.lines[index] = plan.lines[--plan.count];
  }
  if (plan.count == plan.lines.size()) {
    plan.truncated = true;
    return false;
  }
  plan.lines[plan.count++] = candidate;
  return true;
}

bool wordGeometry(GfxRenderer& renderer, const PageLine& line, const TextBlock& block, const uint16_t wordIndex,
                  const int fontId, const int marginLeft, const int marginTop, const bool backgroundSafe,
                  HighlightLine& geometry) {
  const std::string_view text(block.wordText(wordIndex), block.wordTextLen(wordIndex));
  const int lineHeight = renderer.getLineHeight(fontId);
  const int left = line.xPos + block.wordXpos(wordIndex) + marginLeft;
  int width = renderer.getTextAdvanceX(fontId, block.wordText(wordIndex), block.wordStyle(wordIndex));
  if (block.focusBoundary(wordIndex) > 0) {
    const size_t suffixOffset = std::min<size_t>(block.focusBoundary(wordIndex), text.size());
    width = static_cast<int>(block.focusSuffixX(wordIndex)) +
            std::max(0, renderer.getTextAdvanceX(fontId, block.wordText(wordIndex) + suffixOffset,
                                                 block.wordStyle(wordIndex)));
  }
  const int right = std::min(renderer.getScreenWidth(), left + std::max(1, width));
  const int top = std::clamp(line.yPos + marginTop, 0, renderer.getScreenHeight() - 1);
  const int bottom = std::clamp(line.yPos + marginTop + lineHeight - 1, 0, renderer.getScreenHeight() - 1);
  const int underline = std::max(top, bottom - 1);
  if (right <= left || right <= 0 || left >= renderer.getScreenWidth() || bottom < top) return false;
  geometry = {static_cast<int16_t>(std::max(0, left)),
              static_cast<int16_t>(right - 1),
              static_cast<int16_t>(top),
              static_cast<int16_t>(bottom),
              static_cast<int16_t>(underline),
              backgroundSafe};
  return true;
}

template <typename Selected>
HighlightPlan buildGeometry(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                            const int marginTop, Selected&& selected) {
  HighlightPlan plan;
  if (renderer.getLineHeight(fontId) <= 0) return plan;
  uint32_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    const bool backgroundSafe = !block->getBlockStyle().isRtl;
    HighlightLine run;
    bool hasRun = false;
    const auto flushRun = [&]() {
      if (!hasRun) return true;
      hasRun = false;
      return addHighlightLine(plan, run);
    };
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const std::string_view text(block->wordText(i), block->wordTextLen(i));
      if (!hasVisibleText(text)) continue;
      if (!selected(*block, i, pageWordIndex++)) {
        if (!flushRun()) return plan;
        continue;
      }
      HighlightLine word;
      if (!wordGeometry(renderer, line, *block, i, fontId, marginLeft, marginTop, backgroundSafe, word)) continue;
      if (!hasRun) {
        run = word;
        hasRun = true;
      } else {
        // Selected words are contiguous in reading order on this visual line.
        // Expanding one band across their geometry also fills the natural word
        // spacing without joining highlights separated by an unselected word.
        run.left = std::min(run.left, word.left);
        run.right = std::max(run.right, word.right);
        run.top = std::min(run.top, word.top);
        run.bottom = std::max(run.bottom, word.bottom);
        run.y = std::max(run.y, word.y);
      }
    }
    if (!flushRun()) return plan;
  }
  return plan;
}

}  // namespace

bool hasVisibleText(const std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    const size_t whitespaceBytes = whitespaceBytesAt(text, offset);
    if (whitespaceBytes == 0) return true;
    offset += whitespaceBytes;
  }
  return false;
}

uint32_t fingerprint(const Page& page, const GfxRenderer& renderer, const int fontId, const int marginLeft,
                     const int marginTop) {
  return fingerprintImpl(page, fontId, renderer.getScreenWidth(), renderer.getScreenHeight(),
                         renderer.getLineHeight(fontId), marginLeft, marginTop);
}

uint32_t layoutFingerprint(const LayoutIdentity& identity) {
  uint32_t checksum = 0;
  uint32_t lineCompression = 0;
  static_assert(sizeof(lineCompression) == sizeof(identity.lineCompression));
  std::memcpy(&lineCompression, &identity.lineCompression, sizeof(lineCompression));
  addU32(checksum, static_cast<uint32_t>(identity.fontId));
  addU32(checksum, lineCompression);
  addU8(checksum, identity.extraParagraphSpacing ? 1 : 0);
  addU8(checksum, identity.paragraphAlignment);
  addU16(checksum, identity.viewportWidth);
  addU16(checksum, identity.viewportHeight);
  addU8(checksum, identity.hyphenationEnabled ? 1 : 0);
  addU8(checksum, identity.embeddedStyle ? 1 : 0);
  addU8(checksum, identity.imageRendering);
  addU8(checksum, identity.focusReadingEnabled ? 1 : 0);
  addU8(checksum, identity.wordSpacing);
  addU8(checksum, identity.renderMode);
  addU8(checksum, identity.forceParagraphIndents ? 1 : 0);
  return checksum == 0 ? 1 : checksum;
}

// cppcheck-suppress constParameterReference; keep the public drawing API's mutable renderer reference.
void HighlightPlan::drawInverse(GfxRenderer& renderer) const {
  for (size_t i = 0; i < count; ++i) {
    const HighlightLine& line = lines[i];
    if (!line.backgroundSafe) continue;
    renderer.invertRect(line.left, line.top, line.right - line.left + 1, line.bottom - line.top + 1);
  }
}

// cppcheck-suppress constParameterReference; keep the public drawing API's mutable renderer reference.
void HighlightPlan::clearGrayscale(GfxRenderer& renderer) const {
  for (size_t i = 0; i < count; ++i) {
    const HighlightLine& line = lines[i];
    if (!line.backgroundSafe) continue;
    renderer.fillRect(line.left, line.top, line.right - line.left + 1, line.bottom - line.top + 1, true);
  }
}

// cppcheck-suppress constParameterReference; keep the public drawing API's mutable renderer reference.
void HighlightPlan::drawUnderline(GfxRenderer& renderer, const bool fallbackOnly) const {
  for (size_t i = 0; i < count; ++i) {
    if (fallbackOnly && lines[i].backgroundSafe) continue;
    renderer.drawLine(lines[i].left, lines[i].y, lines[i].right, lines[i].y, 2, true);
  }
}

HighlightPlan buildHighlightPlan(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                                 const int marginTop, const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                 const uint16_t spineIndex, const uint16_t pageIndex, const uint32_t pageFingerprint,
                                 const uint32_t currentLayoutFingerprint, const size_t maxHighlights) {
  std::array<WordRange, ClippingCodec::MAX_CLIPPINGS_PER_BOOK> ranges{};
  std::array<SourceWordAnchor, ClippingCodec::MAX_CLIPPINGS_PER_BOOK> sourceRanges{};
  size_t rangeCount = 0;
  size_t sourceRangeCount = 0;
  const size_t scanLimit = std::min(clippings.size(), static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  const size_t matchLimit = std::min(maxHighlights, static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  for (size_t index = 0; index < scanLimit && rangeCount + sourceRangeCount < matchLimit; ++index) {
    const auto& clipping = clippings[index];
    if (clipping.spineIndex != spineIndex) continue;
    if (clipping.hasTextAnchor && clipping.textSourceStart < clipping.textSourceEnd) {
      sourceRanges[sourceRangeCount++] = {clipping.textSourceStart, clipping.textSourceEnd};
      continue;
    }
    if (pageIndex < clipping.startPage || pageIndex > clipping.endPage) continue;

    const bool layoutMatch = clipping.layoutFingerprint != 0 && currentLayoutFingerprint != 0 &&
                             clipping.layoutFingerprint == currentLayoutFingerprint;
    const bool legacyExactMatch = clipping.layoutFingerprint == 0 && clipping.startPage == pageIndex &&
                                  clipping.endPage == pageIndex && clipping.pageFingerprint != 0 &&
                                  clipping.pageFingerprint == pageFingerprint;
    if (!layoutMatch && !legacyExactMatch) continue;

    WordRange range;
    range.first = pageIndex == clipping.startPage ? clipping.startWordIndex : 0;
    range.last = pageIndex == clipping.endPage ? clipping.endWordIndex : std::numeric_limits<uint32_t>::max();
    if (range.first <= range.last) ranges[rangeCount++] = range;
  }
  return buildGeometry(
      renderer, page, fontId, marginLeft, marginTop,
      [&](const TextBlock& block, const uint16_t blockWordIndex, const uint32_t pageWordIndex) {
        const bool pageMatch = std::any_of(
            ranges.begin(), ranges.begin() + static_cast<std::ptrdiff_t>(rangeCount),
            [&](const WordRange& range) { return pageWordIndex >= range.first && pageWordIndex <= range.last; });
        if (pageMatch || !block.hasSourceAnchor(blockWordIndex)) return pageMatch;
        const uint32_t start = block.sourceStart(blockWordIndex);
        const uint32_t end = block.sourceEnd(blockWordIndex);
        return std::any_of(sourceRanges.begin(), sourceRanges.begin() + static_cast<std::ptrdiff_t>(sourceRangeCount),
                           [&](const SourceWordAnchor& range) { return start < range.end && range.start < end; });
      });
}

HighlightPlan buildExactHighlightPlan(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                                      const int marginTop,
                                      const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                      const uint16_t spineIndex, const uint16_t pageIndex,
                                      const uint32_t pageFingerprint, const size_t maxHighlights) {
  return buildHighlightPlan(renderer, page, fontId, marginLeft, marginTop, clippings, spineIndex, pageIndex,
                            pageFingerprint, 0, maxHighlights);
}

HighlightPlan buildTextAnchorHighlightPlan(GfxRenderer& renderer, const Page& page, const int fontId,
                                           const int marginLeft, const int marginTop, const SourceWordAnchor* anchors,
                                           const size_t anchorCount,
                                           const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                           const size_t maxHighlights) {
  if ((!anchors && anchorCount != 0) || maxHighlights == 0) return {};
  size_t visibleWords = 0;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t word = 0; word < block->wordCount(); ++word) {
      if (hasVisibleText({block->wordText(word), block->wordTextLen(word)})) ++visibleWords;
    }
  }
  if (visibleWords != anchorCount) return {};

  std::array<SourceWordAnchor, ClippingCodec::MAX_CLIPPINGS_PER_BOOK> ranges{};
  size_t rangeCount = 0;
  const size_t scanLimit = std::min(clippings.size(), static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  const size_t matchLimit = std::min(maxHighlights, static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  for (size_t index = 0; index < scanLimit && rangeCount < matchLimit; ++index) {
    const auto& clipping = clippings[index];
    if (clipping.hasTextAnchor && clipping.textSourceStart < clipping.textSourceEnd) {
      ranges[rangeCount++] = {clipping.textSourceStart, clipping.textSourceEnd};
    }
  }
  return buildGeometry(
      renderer, page, fontId, marginLeft, marginTop, [&](const TextBlock&, const uint16_t, const uint32_t wordIndex) {
        if (wordIndex >= anchorCount || anchors[wordIndex].start >= anchors[wordIndex].end) return false;
        return std::any_of(ranges.begin(), ranges.begin() + static_cast<std::ptrdiff_t>(rangeCount),
                           [&](const SourceWordAnchor& range) {
                             return anchors[wordIndex].start < range.end && range.start < anchors[wordIndex].end;
                           });
      });
}

std::string normalizeReanchorText(const std::string_view text) {
  if (text.empty() || text.size() > ClippingCodec::MAX_TEXT_BYTES || !ClippingCodec::isValidUtf8(text)) return {};
  const std::string composed = utf8ComposeNfc(std::string(text));
  std::string normalized;
  normalized.reserve(composed.size());
  bool pendingSpace = false;
  for (size_t offset = 0; offset < composed.size();) {
    const size_t spaceBytes = whitespaceBytesAt(composed, offset);
    if (spaceBytes != 0) {
      pendingSpace = !normalized.empty();
      offset += spaceBytes;
      continue;
    }
    if (pendingSpace) normalized.push_back(' ');
    pendingSpace = false;
    const uint8_t first = static_cast<uint8_t>(composed[offset]);
    size_t bytes = 1;
    if ((first & 0xE0U) == 0xC0U) bytes = 2;
    if ((first & 0xF0U) == 0xE0U) bytes = 3;
    if ((first & 0xF8U) == 0xF0U) bytes = 4;
    normalized.append(composed, offset, bytes);
    offset += bytes;
  }
  return normalized;
}

ExactReanchorMatcher::ExactReanchorMatcher(const std::string_view target, const size_t maxPages,
                                           const Cancellation cancellation)
    : target_(normalizeReanchorText(target)), cancellation_(cancellation), maxPages_(maxPages) {
  if (target_.empty()) {
    status_ = ReanchorStatus::InvalidInput;
    return;
  }
  if (maxPages_ == 0 || maxPages_ > MAX_REANCHOR_PAGES || target_.size() > MAX_TARGET_BYTES) {
    status_ = ReanchorStatus::LimitExceeded;
    return;
  }
  for (size_t i = 1, matched = 0; i < target_.size(); ++i) {
    while (matched > 0 && target_[i] != target_[matched]) matched = prefix_[matched - 1];
    if (target_[i] == target_[matched]) ++matched;
    prefix_[i] = static_cast<uint16_t>(matched);
  }
}

bool ExactReanchorMatcher::cancelled() const {
  return cancellation_.requested && cancellation_.requested(cancellation_.context);
}

void ExactReanchorMatcher::feedByte(const char byte, const Position position) {
  while (matchedBytes_ > 0 && byte != target_[matchedBytes_]) matchedBytes_ = prefix_[matchedBytes_ - 1];
  if (byte == target_[matchedBytes_]) ++matchedBytes_;
  positions_[emittedBytes_ % target_.size()] = position;
  ++emittedBytes_;
  if (matchedBytes_ != target_.size()) return;

  const Position& start = positions_[(emittedBytes_ - target_.size()) % target_.size()];
  if (!start.wordStart || !position.wordEnd) {
    matchedBytes_ = prefix_[matchedBytes_ - 1];
    return;
  }
  ++matchCount_;
  if (matchCount_ == 1) {
    const uint32_t words = position.ordinal - start.ordinal + 1;
    if (words > std::numeric_limits<uint16_t>::max()) {
      status_ = ReanchorStatus::LimitExceeded;
      return;
    }
    match_.status = ReanchorStatus::Found;
    match_.startPage = start.page;
    match_.endPage = position.page;
    match_.startWordIndex = start.word;
    match_.endWordIndex = position.word;
    match_.wordCount = static_cast<uint16_t>(words);
    match_.startPageFingerprint = start.pageFingerprint;
  } else {
    status_ = ReanchorStatus::Ambiguous;
  }
  matchedBytes_ = prefix_[matchedBytes_ - 1];
}

bool ExactReanchorMatcher::feedWord(const std::string_view word, const Position& position, const int lineY, const int x,
                                    const int width) {
  const bool paragraphBoundary = startsParagraph(word);
  const std::string normalized = normalizeReanchorText(word);
  if (normalized.empty()) return true;
  if (normalized.size() > MAX_TARGET_BYTES) {
    status_ = ReanchorStatus::LimitExceeded;
    return false;
  }

  const bool sameLine = previousPage_ == static_cast<int>(position.page) && previousLineY_ == lineY;
  const bool visuallyAttached =
      sameLine && x <= previousEndX_ && !(wordLikeBoundary(previousWord_, false) && wordLikeBoundary(normalized, true));
  const bool authoredHyphenJoin =
      !previousWord_.empty() && previousWord_.back() == '-' && wordLikeBoundary(normalized, true);
  if (!previousWord_.empty() &&
      (paragraphBoundary || (!isClosingPunctuation(normalized) && !isOpeningPunctuation(previousWord_) &&
                             !authoredHyphenJoin && !visuallyAttached))) {
    feedByte(' ', position);
  }
  for (size_t index = 0; index < normalized.size(); ++index) {
    Position bytePosition = position;
    bytePosition.wordStart = index == 0;
    bytePosition.wordEnd = index + 1 == normalized.size();
    feedByte(normalized[index], bytePosition);
    if (status_ == ReanchorStatus::Ambiguous || status_ == ReanchorStatus::LimitExceeded) return false;
  }
  previousWord_ = normalized;
  previousPage_ = position.page;
  previousLineY_ = lineY;
  previousEndX_ = x + std::max(0, width);
  return true;
}

ReanchorStatus ExactReanchorMatcher::feedPage(const uint16_t pageIndex, const uint32_t pageFingerprint,
                                              const Page& page, const GfxRenderer& renderer, const int fontId) {
  if (status_ != ReanchorStatus::Ready) return status_;
  if (pagesFed_ >= maxPages_) return status_ = ReanchorStatus::LimitExceeded;
  if (cancelled()) return status_ = ReanchorStatus::Cancelled;
  ++pagesFed_;

  uint16_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (cancelled()) return status_ = ReanchorStatus::Cancelled;
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t word = 0; word < block->wordCount(); ++word) {
      if (cancelled()) return status_ = ReanchorStatus::Cancelled;
      const std::string_view text(block->wordText(word), block->wordTextLen(word));
      if (!hasVisibleText(text)) continue;
      if (wordOrdinal_ == std::numeric_limits<uint32_t>::max()) return status_ = ReanchorStatus::LimitExceeded;
      const Position position{pageIndex, pageWordIndex++, wordOrdinal_++, pageFingerprint};
      const int width = renderer.getTextAdvanceX(fontId, block->wordText(word), block->wordStyle(word));
      if (!feedWord(text, position, line.yPos, line.xPos + block->wordXpos(word), width)) return status_;
    }
  }
  return status_;
}

ReanchorResult ExactReanchorMatcher::finish() const {
  if (status_ != ReanchorStatus::Ready) {
    ReanchorResult result = match_;
    result.status = status_;
    return result;
  }
  if (matchCount_ == 1) return match_;
  ReanchorResult result;
  result.status = matchCount_ == 0 ? ReanchorStatus::NotFound : ReanchorStatus::Ambiguous;
  return result;
}

}  // namespace ClippingPageTools
