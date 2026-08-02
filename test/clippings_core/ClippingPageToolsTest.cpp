#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ClippingPageTools.h"

namespace {

Page makePage(const int focusSuffix = 0, const int leftMargin = 0, const bool withImage = false) {
  BlockStyle style;
  style.marginLeft = static_cast<int16_t>(leftMargin);
  style.textAlignDefined = true;
  auto block =
      std::make_shared<TextBlock>(std::vector<std::string>{"Xin", "chào"}, std::vector<int16_t>{0, 30},
                                  std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR, EpdFontFamily::BOLD},
                                  std::vector<uint8_t>{0, static_cast<uint8_t>(focusSuffix == 0 ? 0 : 2)},
                                  std::vector<uint16_t>{0, static_cast<uint16_t>(focusSuffix)}, style);

  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  if (withImage) page.elements.push_back(std::make_shared<PageImage>(80, 60, 12, 90));
  return page;
}

Page makeGridPage(const size_t count) {
  Page page;
  for (size_t index = 0; index < count; ++index) {
    auto block = std::make_shared<TextBlock>(std::vector<std::string>{"x"}, std::vector<int16_t>{0},
                                             std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR});
    page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, static_cast<int>(index * 3)));
  }
  return page;
}

Page makeTextPage(std::vector<std::string> words, const bool rtl = false) {
  std::vector<int16_t> positions(words.size());
  for (size_t index = 0; index < positions.size(); ++index) positions[index] = static_cast<int16_t>(index * 40);
  std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  BlockStyle style;
  style.isRtl = rtl;
  auto block = std::make_shared<TextBlock>(std::move(words), std::move(positions), std::move(styles),
                                           std::vector<uint8_t>{}, std::vector<uint16_t>{}, style);
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  return page;
}

Page makeAnchoredTextPage(std::vector<std::string> words, std::vector<uint32_t> starts, std::vector<uint32_t> ends) {
  std::vector<int16_t> positions(words.size());
  for (size_t index = 0; index < positions.size(); ++index) positions[index] = static_cast<int16_t>(index * 40);
  std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  auto block =
      std::make_shared<TextBlock>(std::move(words), std::move(positions), std::move(styles), std::vector<uint8_t>{},
                                  std::vector<uint16_t>{}, BlockStyle{}, std::move(starts), std::move(ends));
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  return page;
}

ClippingCodec::ClippingMetadata clippingForWord(const uint16_t word, const uint32_t fingerprint) {
  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 3;
  clipping.pageCount = 10;
  clipping.startWordIndex = word;
  clipping.endWordIndex = word;
  clipping.wordCount = 1;
  clipping.textLength = 1;
  clipping.pageFingerprint = fingerprint;
  return clipping;
}

}  // namespace

TEST(ClippingPageTools, FingerprintCoversRenderContextAndAvailableLayoutData) {
  const Page page = makePage();
  const GfxRenderer renderer(480, 800, 24);
  const uint32_t baseline = ClippingPageTools::fingerprint(page, renderer, 101, 12, 18);
  ASSERT_NE(baseline, 0U);

  EXPECT_NE(ClippingPageTools::fingerprint(page, renderer, 102, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, GfxRenderer(480, 800, 25), 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, GfxRenderer(800, 480, 24), 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, renderer, 101, 13, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(9), renderer, 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(0, 4), renderer, 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(0, 0, true), renderer, 101, 12, 18), baseline);
}

TEST(ClippingPageTools, LayoutFingerprintCoversEverySectionPaginationInput) {
  ClippingPageTools::LayoutIdentity identity;
  identity.fontId = 101;
  identity.lineCompression = 1.2F;
  identity.extraParagraphSpacing = true;
  identity.paragraphAlignment = 2;
  identity.viewportWidth = 456;
  identity.viewportHeight = 760;
  identity.hyphenationEnabled = true;
  identity.embeddedStyle = true;
  identity.imageRendering = 1;
  identity.focusReadingEnabled = true;
  identity.renderMode = 2;
  identity.forceParagraphIndents = true;
  const uint32_t baseline = ClippingPageTools::layoutFingerprint(identity);
  ASSERT_NE(baseline, 0U);

  const auto expectChanged = [&](auto member, const auto value) {
    auto changed = identity;
    changed.*member = value;
    EXPECT_NE(ClippingPageTools::layoutFingerprint(changed), baseline);
  };
  expectChanged(&ClippingPageTools::LayoutIdentity::fontId, 102);
  expectChanged(&ClippingPageTools::LayoutIdentity::lineCompression, 1.3F);
  expectChanged(&ClippingPageTools::LayoutIdentity::extraParagraphSpacing, false);
  expectChanged(&ClippingPageTools::LayoutIdentity::paragraphAlignment, uint8_t{3});
  expectChanged(&ClippingPageTools::LayoutIdentity::viewportWidth, uint16_t{455});
  expectChanged(&ClippingPageTools::LayoutIdentity::viewportHeight, uint16_t{759});
  expectChanged(&ClippingPageTools::LayoutIdentity::hyphenationEnabled, false);
  expectChanged(&ClippingPageTools::LayoutIdentity::embeddedStyle, false);
  expectChanged(&ClippingPageTools::LayoutIdentity::imageRendering, uint8_t{2});
  expectChanged(&ClippingPageTools::LayoutIdentity::focusReadingEnabled, false);
  expectChanged(&ClippingPageTools::LayoutIdentity::renderMode, uint8_t{1});
  expectChanged(&ClippingPageTools::LayoutIdentity::forceParagraphIndents, false);
}

TEST(ClippingPageTools, BuildsHighlightsOnlyForTheExactFingerprint) {
  const Page page = makePage();
  GfxRenderer renderer(480, 800, 24);
  const uint32_t fingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 12, 18);

  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 3;
  clipping.pageCount = 10;
  clipping.startWordIndex = 0;
  clipping.endWordIndex = 1;
  clipping.wordCount = 2;
  clipping.textLength = 1;
  clipping.pageFingerprint = fingerprint;

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 12, 18, {clipping}, 2, 3, fingerprint);
  ASSERT_EQ(plan.count, 2U);
  EXPECT_EQ(plan.lines[0].left, 17);
  EXPECT_EQ(plan.lines[0].y, 60);
  EXPECT_EQ(plan.lines[1].left, 47);

  EXPECT_EQ(
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 12, 18, {clipping}, 2, 3, fingerprint ^ 1U).count,
      0U);
}

TEST(ClippingPageTools, BuildsFirstMiddleAndLastRangesForAMultiPageHighlight) {
  const Page page = makeGridPage(5);
  GfxRenderer renderer(480, 800, 20);
  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 5;
  clipping.pageCount = 10;
  clipping.startWordIndex = 2;
  clipping.endWordIndex = 1;
  clipping.wordCount = 9;
  clipping.textLength = 1;
  clipping.layoutFingerprint = 0x12345678U;

  const auto first =
      ClippingPageTools::buildHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 3, 1, clipping.layoutFingerprint);
  const auto middle =
      ClippingPageTools::buildHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 4, 2, clipping.layoutFingerprint);
  const auto last =
      ClippingPageTools::buildHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 5, 3, clipping.layoutFingerprint);
  EXPECT_EQ(first.count, 3U);
  EXPECT_EQ(middle.count, 5U);
  EXPECT_EQ(last.count, 2U);
  EXPECT_EQ(ClippingPageTools::buildHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 4, 2,
                                                  clipping.layoutFingerprint ^ 1U)
                .count,
            0U);
}

TEST(ClippingPageTools, MergesOverlappingGeometryAndOffersBackgroundAndUnderlineFallback) {
  auto block =
      std::make_shared<TextBlock>(std::vector<std::string>{"abc", "def"}, std::vector<int16_t>{0, 6},
                                  std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR, EpdFontFamily::REGULAR});
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  GfxRenderer renderer(480, 800, 24);
  const uint32_t exact = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);
  auto clipping = clippingForWord(0, exact);
  clipping.endWordIndex = 1;
  clipping.wordCount = 2;
  const auto plan = ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 3, exact);
  ASSERT_EQ(plan.count, 1U);
  EXPECT_EQ(plan.lines[0].left, 5);
  EXPECT_EQ(plan.lines[0].right, 28);
  plan.drawBackground(renderer);
  plan.drawUnderline(renderer);
  EXPECT_EQ(renderer.backgroundCount(), 1);
  EXPECT_EQ(renderer.underlineCount(), 1);

  const Page rtlPage = makeTextPage({"phải"}, true);
  const uint32_t rtlExact = ClippingPageTools::fingerprint(rtlPage, renderer, 101, 0, 0);
  const auto rtl = ClippingPageTools::buildExactHighlightPlan(renderer, rtlPage, 101, 0, 0,
                                                              {clippingForWord(0, rtlExact)}, 2, 3, rtlExact);
  ASSERT_EQ(rtl.count, 1U);
  EXPECT_FALSE(rtl.lines[0].backgroundSafe);
  rtl.drawBackground(renderer);
  rtl.drawUnderline(renderer, true);
  EXPECT_EQ(renderer.backgroundCount(), 1);
  EXPECT_EQ(renderer.underlineCount(), 2);
}

TEST(ClippingPageTools, BuildsTxtHighlightsOnlyFromExactSourceAnchorOverlap) {
  const Page page = makeTextPage({"một", "hai", "ba"});
  GfxRenderer renderer(480, 800, 24);
  const std::array<ClippingPageTools::SourceWordAnchor, 3> anchors = {ClippingPageTools::SourceWordAnchor{100, 103},
                                                                      ClippingPageTools::SourceWordAnchor{104, 107},
                                                                      ClippingPageTools::SourceWordAnchor{108, 110}};
  ClippingCodec::ClippingMetadata clipping;
  clipping.hasTextAnchor = true;
  clipping.textSourceStart = 105;
  clipping.textSourceEnd = 109;
  clipping.startPage = 0;
  clipping.endPage = 0;
  clipping.pageCount = 1;
  clipping.wordCount = 2;
  clipping.textLength = 1;

  const auto plan = ClippingPageTools::buildTextAnchorHighlightPlan(renderer, page, 101, 0, 0, anchors.data(),
                                                                    anchors.size(), {clipping});
  EXPECT_EQ(plan.count, 2U);
  EXPECT_EQ(
      ClippingPageTools::buildTextAnchorHighlightPlan(renderer, page, 101, 0, 0, anchors.data(), 2, {clipping}).count,
      0U);
}

TEST(ClippingPageTools, EpubTextAnchorsSurvivePageAndLayoutChanges) {
  const Page page = makeAnchoredTextPage({"alpha", "beta", "gamma"}, {0, 5, 9}, {5, 9, 14});
  GfxRenderer renderer(480, 800, 24);
  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 8;
  clipping.endPage = 8;
  clipping.pageCount = 12;
  clipping.startWordIndex = 7;
  clipping.endWordIndex = 8;
  clipping.wordCount = 2;
  clipping.textLength = 9;
  clipping.pageFingerprint = 0x11223344U;
  clipping.layoutFingerprint = 0x55667788U;
  clipping.hasTextAnchor = true;
  clipping.textSourceStart = 6;
  clipping.textSourceEnd = 12;

  const auto plan =
      ClippingPageTools::buildHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 1, 0xDEADBEEFU, 0xCAFEBABEU);
  EXPECT_EQ(plan.count, 2U);
  EXPECT_EQ(plan.lines[0].left, 45);
  EXPECT_EQ(plan.lines[1].left, 85);
}

TEST(ClippingPageTools, UsesFocusSplitGeometryForBoldPrefixAndRegularSuffix) {
  auto block = std::make_shared<TextBlock>(std::vector<std::string>{"abcdef"}, std::vector<int16_t>{0},
                                           std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR},
                                           std::vector<uint8_t>{2}, std::vector<uint16_t>{18});
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  GfxRenderer renderer(480, 800, 24);
  const uint32_t fingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);

  const auto plan = ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0,
                                                               {clippingForWord(0, fingerprint)}, 2, 3, fingerprint);

  ASSERT_EQ(plan.count, 1U);
  EXPECT_EQ(plan.lines[0].left, 5);
  // The stub's regular suffix is 4 * 6 px; its bold-prefix layout offset is 18 px.
  EXPECT_EQ(plan.lines[0].right, 5 + 18 + 4 * 6 - 1);
}

TEST(ClippingPageTools, HighlightsTheSeventeenthAndAllSixtyFourStoredClippings) {
  const Page page = makeGridPage(ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  GfxRenderer renderer(480, 800, 24);
  const uint32_t pageFingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);
  std::vector<ClippingCodec::ClippingMetadata> clippings;
  clippings.reserve(ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  for (uint16_t word = 0; word < ClippingCodec::MAX_CLIPPINGS_PER_BOOK; ++word) {
    clippings.push_back(clippingForWord(word, pageFingerprint));
  }

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, clippings, 2, 3, pageFingerprint);
  EXPECT_EQ(plan.count, ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  EXPECT_FALSE(plan.truncated);
  EXPECT_EQ(plan.lines[16].left, 5);

  clippings[16].pageFingerprint ^= 1U;
  const auto mismatched =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, clippings, 2, 3, pageFingerprint);
  EXPECT_EQ(mismatched.count, ClippingCodec::MAX_CLIPPINGS_PER_BOOK - 1);
}

TEST(ClippingPageTools, ReportsGeometryTruncationAtItsFixedMemoryLimit) {
  const Page page = makeGridPage(ClippingPageTools::HighlightPlan::MAX_LINES + 1);
  GfxRenderer renderer(480, 800, 2);
  const uint32_t pageFingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);
  auto clipping = clippingForWord(0, pageFingerprint);
  clipping.endWordIndex = static_cast<uint16_t>(ClippingPageTools::HighlightPlan::MAX_LINES);
  clipping.wordCount = static_cast<uint16_t>(ClippingPageTools::HighlightPlan::MAX_LINES + 1);

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 3, pageFingerprint);
  EXPECT_EQ(plan.count, ClippingPageTools::HighlightPlan::MAX_LINES);
  EXPECT_TRUE(plan.truncated);
}

TEST(ClippingPageTools, TruncationNoticeIsOncePerRecentPageWithinFixedMemory) {
  ClippingPageTools::HighlightNoticeTracker tracker;
  EXPECT_TRUE(tracker.markIfNew(2, 3, 100));
  EXPECT_FALSE(tracker.markIfNew(2, 3, 100));
  EXPECT_TRUE(tracker.markIfNew(2, 3, 101));  // changed layout is a new identity
  EXPECT_TRUE(tracker.markIfNew(2, 4, 100));
  EXPECT_FALSE(tracker.markIfNew(0, 0, 0));

  for (size_t i = 3; i < ClippingPageTools::HighlightNoticeTracker::MAX_TRACKED_PAGES + 1; ++i) {
    EXPECT_TRUE(tracker.markIfNew(7, static_cast<uint16_t>(i), static_cast<uint32_t>(1000 + i)));
  }
  // The oldest identity can be reported again after the fixed-size ring has
  // evicted it; memory use never grows with page count.
  EXPECT_TRUE(tracker.markIfNew(2, 3, 100));
}

TEST(ClippingPageTools, PageAdvanceRequiresCompleteExtraction) {
  ClippingPageTools::SelectionPageAdvanceState state;
  state.loaderAvailable = true;
  state.extractionComplete = true;
  state.selectionStarted = true;
  state.wordCount = 192;
  state.cursorOrder = 191;
  state.currentPage = 3;
  state.pageCount = 5;
  state.selectionFits = true;
  EXPECT_TRUE(ClippingPageTools::canAdvanceSelectionPage(state));

  // A dense page may contain uncaptured words after the 192-word bounded
  // selection window. Advancing from that window would create a non-contiguous
  // clipping, so both the transition and its UI affordance must remain off.
  state.extractionComplete = false;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));

  state.extractionComplete = true;
  state.cursorOrder = 190;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));
  state.cursorOrder = 191;
  state.currentPage = 4;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));
}

TEST(ClippingPageTools, CrossPageSelectionMustCoverAContiguousPhysicalPageTail) {
  const uint16_t completeTail[] = {18, 19, 20, 21};
  EXPECT_TRUE(ClippingPageTools::isContiguousTail(completeTail, std::size(completeTail), 22));

  const uint16_t hiddenMiddle[] = {18, 20, 21};
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(hiddenMiddle, std::size(hiddenMiddle), 22));

  const uint16_t hiddenEnd[] = {18, 19, 20};
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(hiddenEnd, std::size(hiddenEnd), 22));
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(nullptr, 0, 22));
}

TEST(ClippingPageTools, ReanchorMatchesNfcAndCollapsedWhitespaceAcrossPages) {
  GfxRenderer renderer;
  ClippingPageTools::ExactReanchorMatcher matcher("cuối  trang\nđâ\xCC\x80u sau", 2);
  EXPECT_EQ(matcher.feedPage(4, 0x1111U, makeTextPage({"cuối", "trang"}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Ready);
  EXPECT_EQ(matcher.feedPage(5, 0x2222U, makeTextPage({"đầu", "sau"}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Ready);
  const auto result = matcher.finish();
  EXPECT_EQ(result.status, ClippingPageTools::ReanchorStatus::Found);
  EXPECT_EQ(result.startPage, 4);
  EXPECT_EQ(result.endPage, 5);
  EXPECT_EQ(result.startWordIndex, 0);
  EXPECT_EQ(result.endWordIndex, 1);
  EXPECT_EQ(result.wordCount, 4);
  EXPECT_EQ(result.startPageFingerprint, 0x1111U);
}

TEST(ClippingPageTools, ReanchorPreservesParagraphBoundaryBeforeDialoguePunctuation) {
  const std::string dialogue = "\xE2\x80\x83\"Xin";
  GfxRenderer renderer;
  ClippingPageTools::ExactReanchorMatcher matcher("cuối\n\"Xin", 1);
  ASSERT_EQ(matcher.feedPage(0, 0x1234U, makeTextPage({"cuối", dialogue}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Ready);
  const auto result = matcher.finish();
  EXPECT_EQ(result.status, ClippingPageTools::ReanchorStatus::Found);
  EXPECT_EQ(result.startWordIndex, 0);
  EXPECT_EQ(result.endWordIndex, 1);
}

TEST(ClippingPageTools, ReanchorRejectsAmbiguousMatchesAndHonorsBoundsAndCancellation) {
  GfxRenderer renderer;
  ClippingPageTools::ExactReanchorMatcher ambiguous("lặp lại", 2);
  EXPECT_EQ(ambiguous.feedPage(1, 1, makeTextPage({"lặp", "lại"}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Ready);
  EXPECT_EQ(ambiguous.feedPage(2, 2, makeTextPage({"lặp", "lại"}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Ambiguous);
  EXPECT_EQ(ambiguous.finish().status, ClippingPageTools::ReanchorStatus::Ambiguous);

  ClippingPageTools::ExactReanchorMatcher tooWide("text", ClippingPageTools::MAX_REANCHOR_PAGES + 1);
  EXPECT_EQ(tooWide.finish().status, ClippingPageTools::ReanchorStatus::LimitExceeded);

  bool cancel = true;
  const ClippingPageTools::Cancellation cancellation{&cancel,
                                                     [](void* context) { return *static_cast<bool*>(context); }};
  ClippingPageTools::ExactReanchorMatcher cancelled("text", 1, cancellation);
  EXPECT_EQ(cancelled.feedPage(1, 1, makeTextPage({"text"}), renderer, 101),
            ClippingPageTools::ReanchorStatus::Cancelled);
  EXPECT_EQ(cancelled.finish().status, ClippingPageTools::ReanchorStatus::Cancelled);
}

TEST(ClippingPageTools, ReanchorMatchesWholeSelectedWordsOnly) {
  GfxRenderer renderer;
  ClippingPageTools::ExactReanchorMatcher matcher("he", 2);
  ASSERT_EQ(matcher.feedPage(1, 11, makeTextPage({"the"}), renderer, 2), ClippingPageTools::ReanchorStatus::Ready);
  ASSERT_EQ(matcher.feedPage(2, 22, makeTextPage({"he"}), renderer, 2), ClippingPageTools::ReanchorStatus::Ready);
  const ClippingPageTools::ReanchorResult result = matcher.finish();
  EXPECT_EQ(result.status, ClippingPageTools::ReanchorStatus::Found);
  EXPECT_EQ(result.startPage, 2);
  EXPECT_EQ(result.endPage, 2);
  EXPECT_EQ(result.startWordIndex, 0);
  EXPECT_EQ(result.endWordIndex, 0);
}
