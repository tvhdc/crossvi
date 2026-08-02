#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>

#include "activities/home/DashboardProgress.h"
#include "activities/home/DashboardStatsPolicy.h"
#include "activities/home/HomeBookSummary.h"
#include "components/themes/HomeMenuLayout.h"
#include "components/themes/crossvi/CrossViLayout.h"
#include "components/themes/crossvi/CrossViTheme.h"

namespace {

bool contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.width >= 0 && inner.height >= 0 &&
         inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}

bool overlaps(const Rect& a, const Rect& b) {
  return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width && a.x + a.width > b.x &&
         a.y < b.y + b.height && a.y + a.height > b.y;
}

}  // namespace

TEST(HomeMenuLayout, PreservesPreferredRhythmWhenItFits) {
  const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(430, 6, 1, 64, 8, 40);
  EXPECT_EQ(layout.rows, 6);
  EXPECT_EQ(layout.rowHeight, 64);
  EXPECT_EQ(layout.rowGap, 8);
  EXPECT_EQ(layout.totalHeight(), 424);
}

TEST(HomeMenuLayout, CompressesEveryRowInsideTheAvailableHeight) {
  for (const int height : {292, 317, 364, 422}) {
    for (const int itemCount : {6, 7, 8}) {
      const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(height, itemCount, 1, 64, 8, 32);
      EXPECT_GT(layout.rowHeight, 0);
      EXPECT_LE(layout.totalHeight(), height);
      EXPECT_EQ(layout.yOffset(itemCount - 1, 1) + layout.rowHeight, layout.totalHeight());
    }
  }
}

TEST(HomeMenuLayout, FitsTwoColumnDashboardMenus) {
  const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(262, 7, 2, 48, 8, 30);
  EXPECT_EQ(layout.rows, 4);
  EXPECT_EQ(layout.rowHeight, 48);
  EXPECT_EQ(layout.rowGap, 8);
  EXPECT_LE(layout.totalHeight(), 262);
  EXPECT_EQ(layout.yOffset(6, 2), 3 * (layout.rowHeight + layout.rowGap));
}

TEST(CrossViLayout, FitsBothSupportedPortraitPanels) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, CrossViMetrics::values.homeCoverTileHeight},
                                             Rect{0, 56, 528, CrossViMetrics::values.homeCoverTileHeight}}) {
    const CrossViLayout layout = CrossViLayout::calculate(tile);

    EXPECT_TRUE(contains(tile, layout.card));
    EXPECT_TRUE(contains(layout.card, layout.cover));
    EXPECT_TRUE(contains(layout.card, layout.details));
    EXPECT_TRUE(contains(layout.details, layout.title));
    EXPECT_TRUE(contains(layout.details, layout.stats));
    EXPECT_TRUE(contains(layout.details, layout.progress));
    EXPECT_TRUE(contains(layout.progress, layout.progressBar));
    EXPECT_TRUE(contains(layout.progress, layout.progressLabel));
    EXPECT_TRUE(contains(layout.details, layout.continueButton));
    EXPECT_TRUE(contains(layout.details, layout.continueButtonWithoutProgress));

    EXPECT_FALSE(overlaps(layout.cover, layout.details));
    EXPECT_FALSE(overlaps(layout.title, layout.stats));
    EXPECT_FALSE(overlaps(layout.stats, layout.progress));
    EXPECT_FALSE(overlaps(layout.progress, layout.continueButton));
    EXPECT_FALSE(overlaps(layout.stats, layout.continueButtonWithoutProgress));
  }
}

TEST(CrossViLayout, CachesOnlyTheCompactCoverRegion) {
  const CrossViLayout x4 = CrossViLayout::calculate(Rect{0, 56, 480, CrossViMetrics::values.homeCoverTileHeight});
  const CrossViLayout x3 = CrossViLayout::calculate(Rect{0, 56, 528, CrossViMetrics::values.homeCoverTileHeight});

  EXPECT_EQ(x4.cover.x - x4.card.x, 20);
  EXPECT_EQ(x3.cover.x - x3.card.x, 20);
  EXPECT_EQ(x4.cover.width, 156);
  EXPECT_EQ(x4.cover.height, 234);
  EXPECT_EQ(x3.cover.width, 156);
  EXPECT_EQ(x3.cover.height, 234);
  EXPECT_EQ(x4.details.x, 212);
  EXPECT_EQ(x4.details.width, 236);
  EXPECT_EQ(x3.details.x, 212);
  EXPECT_EQ(x3.details.width, 284);
  EXPECT_LT(((x4.cover.width + 7) / 8) * x4.cover.height, 5000);
  EXPECT_LT(((x3.cover.width + 7) / 8) * x3.cover.height, 5000);
}

TEST(CrossViRecentListLayout, ProductionTileFitsFourSpaciousRowsAndCapsAtFour) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, CrossViMetrics::HOME_RECENT_LIST_TILE_HEIGHT},
                                             Rect{0, 56, 528, CrossViMetrics::HOME_RECENT_LIST_TILE_HEIGHT}}) {
    EXPECT_EQ(CrossViRecentListLayout::capacity(tile), 4);
    const CrossViRecentListLayout layout = CrossViRecentListLayout::calculate(tile, 9);
    EXPECT_EQ(layout.visibleRows, 4);
    EXPECT_EQ(layout.rowHeight, 70);
    EXPECT_GE(layout.rowHeight, CrossViRecentListLayout::MIN_ROW_HEIGHT);
    EXPECT_LE(layout.rowHeight, CrossViRecentListLayout::MAX_ROW_HEIGHT);
    EXPECT_TRUE(contains(tile, layout.card));
    EXPECT_TRUE(contains(layout.card, layout.header));
    EXPECT_TRUE(contains(layout.card, layout.list));
    for (int row = 0; row < layout.visibleRows; ++row) {
      EXPECT_TRUE(contains(layout.list, layout.row(row)));
      if (row > 0) {
        EXPECT_FALSE(overlaps(layout.row(row - 1), layout.row(row)));
      }
    }
    EXPECT_EQ(layout.card.y + layout.card.height - (layout.list.y + layout.list.height), 10);
  }
}

TEST(CrossViLayout, AdaptiveTwoColumnMenuKeepsSettingsInsideTheSafeArea) {
  for (const int availableHeight : {316, 324, 348, 356}) {
    for (const int itemCount : {6, 7}) {
      const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(availableHeight, itemCount, 2, 80, 10, 48);
      EXPECT_GT(layout.rowHeight, 0);
      EXPECT_LE(layout.totalHeight(), availableHeight);
      EXPECT_LE(layout.yOffset(itemCount - 1, 2) + layout.rowHeight, availableHeight);
    }
  }
}

TEST(CrossViLayout, HomeStylesUseTheAvailableSpaceWithoutLeavingAnEmptyFooter) {
  for (const Rect screen : std::array<Rect, 2>{Rect{0, 0, 528, 792}, Rect{0, 0, 480, 800}}) {
    const int contentBottom =
        screen.height - CrossViMetrics::values.buttonHintsHeight - CrossViMetrics::values.verticalSpacing;
    for (const int tileHeight :
         {CrossViMetrics::values.homeCoverTileHeight, CrossViMetrics::HOME_RECENT_LIST_TILE_HEIGHT}) {
      const int menuTop = CrossViMetrics::values.homeTopPadding + tileHeight + CrossViMetrics::values.homeMenuTopOffset;
      const int availableHeight = contentBottom - menuTop;
      const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(
          availableHeight, 6, 2, CrossViMetrics::values.menuRowHeight, CrossViMetrics::values.menuSpacing, 48);
      const int menuOffset = std::min(24, std::max(0, (availableHeight - layout.totalHeight()) / 2));
      const int footerGap = contentBottom - (menuTop + menuOffset + layout.totalHeight());
      EXPECT_GT(availableHeight, 0);
      EXPECT_GE(footerGap, 0);
      EXPECT_LE(footerGap, 72);
    }
  }
}

TEST(CrossViTripleCoverLayout, FitsThreeLargeEqualBooksOnTheOpenShelf) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, CrossViMetrics::values.homeCoverTileHeight},
                                             Rect{0, 56, 528, CrossViMetrics::values.homeCoverTileHeight}}) {
    const CrossViTripleCoverLayout layout = CrossViTripleCoverLayout::calculate(tile);
    EXPECT_TRUE(contains(tile, layout.card));
    for (int index = 0; index < CrossViTripleCoverLayout::ITEM_COUNT; ++index) {
      EXPECT_TRUE(contains(layout.card, layout.items[index]));
      EXPECT_TRUE(contains(layout.items[index], layout.covers[index]));
      EXPECT_TRUE(contains(layout.items[index], layout.titles[index]));
      EXPECT_EQ(layout.covers[index].width, 144);
      EXPECT_EQ(layout.covers[index].height, 240);
      EXPECT_LT(layout.covers[index].y + layout.covers[index].height, layout.titles[index].y);
      if (index > 0) {
        EXPECT_FALSE(overlaps(layout.items[index - 1], layout.items[index]));
        EXPECT_EQ(layout.covers[index - 1].y, layout.covers[index].y);
      }
    }
    EXPECT_EQ(layout.covers[1].x + layout.covers[1].width / 2, tile.x + tile.width / 2);
  }
}

TEST(CrossViTripleCoverLayout, KeepsBooksInFixedSlotsWhileFocusMoves) {
  EXPECT_EQ(CrossViTripleCoverLayout::fixedBookIndices(0), (std::array<int, 3>{-1, -1, -1}));
  EXPECT_EQ(CrossViTripleCoverLayout::fixedBookIndices(1), (std::array<int, 3>{-1, 0, -1}));
  EXPECT_EQ(CrossViTripleCoverLayout::fixedBookIndices(2), (std::array<int, 3>{0, 1, -1}));
  EXPECT_EQ(CrossViTripleCoverLayout::fixedBookIndices(3), (std::array<int, 3>{0, 1, 2}));

  const auto slots = CrossViTripleCoverLayout::fixedBookIndices(3);
  for (int selectedBook = 0; selectedBook < 3; ++selectedBook) {
    int focusedSlot = -1;
    for (int slot = 0; slot < CrossViTripleCoverLayout::ITEM_COUNT; ++slot) {
      if (slots[slot] == selectedBook) focusedSlot = slot;
    }
    EXPECT_EQ(focusedSlot, selectedBook);
  }
}

TEST(CrossViCarouselLayout, FitsX3AndX4WithoutReplacingTheCrossViHeader) {
  for (const Rect screen : std::array<Rect, 2>{Rect{0, 0, 528, 792}, Rect{0, 0, 480, 800}}) {
    const Rect content{
        0, CrossViMetrics::values.homeTopPadding, screen.width,
        screen.height - CrossViMetrics::values.homeTopPadding - CrossViMetrics::values.buttonHintsHeight};
    const CrossViCarouselLayout layout = CrossViCarouselLayout::calculate(content);

    EXPECT_TRUE(contains(content, layout.coverArea));
    EXPECT_TRUE(contains(layout.coverArea, layout.currentCover));
    EXPECT_TRUE(contains(content, layout.pageDots));
    EXPECT_TRUE(contains(content, layout.author));
    EXPECT_TRUE(contains(content, layout.title));
    EXPECT_TRUE(contains(content, layout.menu));
    EXPECT_EQ(layout.currentCover.width, layout.currentCover.height * 3 / 5);
    EXPECT_TRUE(contains(content, layout.previousCover));
    EXPECT_TRUE(contains(content, layout.nextCover));
    EXPECT_GT(layout.previousCover.width, 0);
    EXPECT_GT(layout.nextCover.width, 0);
    const int leftMargin = layout.previousCover.x - content.x;
    const int rightMargin = content.x + content.width - layout.nextCover.x - layout.nextCover.width;
    const int leftGap = layout.currentCover.x - layout.previousCover.x - layout.previousCover.width;
    const int rightGap = layout.nextCover.x - layout.currentCover.x - layout.currentCover.width;
    EXPECT_EQ(leftMargin, 8);
    EXPECT_EQ(leftMargin, rightMargin);
    EXPECT_EQ(leftGap, 8);
    EXPECT_EQ(leftGap, rightGap);
    EXPECT_LE(layout.currentCover.width, screen.width * 52 / 100);
    EXPECT_FALSE(overlaps(layout.previousCover, layout.currentCover));
    EXPECT_FALSE(overlaps(layout.currentCover, layout.nextCover));
    EXPECT_EQ(layout.previousCover.y, layout.nextCover.y);
    EXPECT_EQ(layout.previousCover.height, layout.nextCover.height);
    EXPECT_FALSE(overlaps(layout.currentCover, layout.pageDots));
    EXPECT_FALSE(overlaps(layout.pageDots, layout.author));
    EXPECT_FALSE(overlaps(layout.author, layout.title));
    EXPECT_FALSE(overlaps(layout.title, layout.menu));
    EXPECT_EQ(layout.menu.y + layout.menu.height, content.y + content.height - 32);
    EXPECT_GE((layout.menu.width - 5 * 6) / 6, 32);
    EXPECT_GE(layout.menu.height, 32);
  }
}

TEST(CrossViCarouselLayout, DedicatedThumbnailCoversBothPhysicalCenterFramesWithoutUpscaling) {
  const CrossViCarouselLayout x3 = CrossViCarouselLayout::calculate(
      Rect{0, CrossViMetrics::values.homeTopPadding, 528,
           792 - CrossViMetrics::values.homeTopPadding - CrossViMetrics::values.buttonHintsHeight});
  const CrossViCarouselLayout x4 = CrossViCarouselLayout::calculate(
      Rect{0, CrossViMetrics::values.homeTopPadding, 480,
           800 - CrossViMetrics::values.homeTopPadding - CrossViMetrics::values.buttonHintsHeight});

  EXPECT_EQ(x3.currentCover.width, 273);
  EXPECT_EQ(x3.currentCover.height, 456);
  EXPECT_EQ(x4.currentCover.width, 249);
  EXPECT_EQ(x4.currentCover.height, 415);
  EXPECT_EQ(CrossViCarouselLayout::THUMBNAIL_HEIGHT, x3.currentCover.height);
  EXPECT_LT(x4.currentCover.height, CrossViCarouselLayout::THUMBNAIL_HEIGHT);
}

TEST(CrossViCarouselLayout, FitsTheOutlineToTheActualThumbnailInsteadOfTheNominalFrame) {
  const Rect fitted = CrossViCarouselLayout::fitCoverWithin(Rect{100, 20, 273, 456}, 273, 414);
  EXPECT_EQ(fitted.x, 100);
  EXPECT_EQ(fitted.y, 41);
  EXPECT_EQ(fitted.width, 273);
  EXPECT_EQ(fitted.height, 414);
}

TEST(CrossViCarouselLayout, ShortensTheSideCoverEdgesNearestTheCenter) {
  constexpr int width = 108;
  EXPECT_EQ(CrossViCarouselLayout::perspectiveColumnInset(1, 0, width), 0);
  EXPECT_EQ(CrossViCarouselLayout::perspectiveColumnInset(1, width - 1, width),
            CrossViCarouselLayout::COVER_PERSPECTIVE_INSET);
  EXPECT_EQ(CrossViCarouselLayout::perspectiveColumnInset(-1, 0, width),
            CrossViCarouselLayout::COVER_PERSPECTIVE_INSET);
  EXPECT_EQ(CrossViCarouselLayout::perspectiveColumnInset(-1, width - 1, width), 0);
  EXPECT_EQ(CrossViCarouselLayout::perspectiveColumnInset(0, width / 2, width), 0);
}

TEST(CrossViCarouselLayout, DoesNotDuplicateSideBooksForSmallLibraries) {
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(0, 0), (std::array<int, 2>{-1, -1}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(0, 1), (std::array<int, 2>{-1, -1}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(0, 2), (std::array<int, 2>{1, -1}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(1, 2), (std::array<int, 2>{0, -1}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(0, 3), (std::array<int, 2>{2, 1}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(1, 3), (std::array<int, 2>{0, 2}));
  EXPECT_EQ(CrossViCarouselLayout::sideBookIndices(2, 3), (std::array<int, 2>{1, 0}));
  EXPECT_EQ(CrossViCarouselLayout::carouselSideBookIndices(0, 2), (std::array<int, 2>{-1, 1}));
  EXPECT_EQ(CrossViCarouselLayout::carouselSideBookIndices(1, 2), (std::array<int, 2>{0, -1}));
  EXPECT_EQ(CrossViCarouselLayout::carouselSideBookIndices(0, 3), (std::array<int, 2>{2, 1}));
}

TEST(CrossViCarouselLayout, DotCountAndSelectionFollowTheCurrentRecentBook) {
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(0, 0), -1);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(0, 1), 0);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(0, 2), 0);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(1, 2), 1);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(0, 3), 0);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(1, 3), 1);
  EXPECT_EQ(CrossViCarouselLayout::selectedDotIndex(2, 3), 2);
}

TEST(CrossViHomeAction, UsesOnlyTrustedExactProgressForItsLabel) {
  HomeBookSummary summary;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Open);

  summary.bookStatsState = DashboardMetricState::NoData;
  summary.progressState = DashboardMetricState::NoData;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Start);

  summary.hasProgress = true;
  summary.progressState = DashboardMetricState::Available;
  summary.progressPercent = 42;
  summary.hasStartedReading = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);

  summary.progressEstimated = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);

  summary.progressEstimated = false;
  summary.progressPercent = 100;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::ReadAgain);
}

TEST(CrossViHomeAction, DoesNotTreatOpeningTheFirstSubPercentPageAsReading) {
  HomeBookSummary summary;
  summary.bookStatsState = DashboardMetricState::NoData;
  summary.progressState = DashboardMetricState::Available;
  summary.hasProgress = true;
  summary.progressBelowOnePercent = true;

  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Start);

  summary.hasStartedReading = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);
}

TEST(DashboardProgress, StrictlyDecodesFinalizedEpubProgress) {
  const std::array<uint8_t, 6> valid{2, 0, 4, 0, 10, 0};
  DashboardProgress::Position position;
  ASSERT_TRUE(DashboardProgress::decode(valid.data(), valid.size(), position));
  EXPECT_EQ(position.spineIndex, 2);
  EXPECT_EQ(position.pageNumber, 4);
  EXPECT_EQ(position.pageCount, 10);

  EXPECT_FALSE(DashboardProgress::decode(valid.data(), 4, position));
  auto invalid = valid;
  invalid[2] = 10;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
  invalid = valid;
  invalid[4] = 0;
  invalid[5] = 0;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
  invalid = valid;
  invalid[2] = 0xFF;
  invalid[3] = 0xFF;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
}

TEST(DashboardProgress, RequiresMatchingFinalizedSectionCache) {
  const DashboardProgress::Position position{2, 4, 10};
  EXPECT_TRUE(DashboardProgress::validate(position, 3, std::optional<uint16_t>{10}));
  EXPECT_FALSE(DashboardProgress::validate(position, 2, std::optional<uint16_t>{10}));
  EXPECT_FALSE(DashboardProgress::validate(position, 3, std::optional<uint16_t>{9}));
  EXPECT_FALSE(DashboardProgress::validate(position, 3, std::nullopt));
}

TEST(DashboardProgress, ConvertsOnlyFiniteProgress) {
  uint8_t percent = 99;
  EXPECT_TRUE(DashboardProgress::toPercent(0.425F, percent));
  EXPECT_EQ(percent, 43);
  EXPECT_TRUE(DashboardProgress::toPercent(-1.0F, percent));
  EXPECT_EQ(percent, 0);
  EXPECT_TRUE(DashboardProgress::toPercent(2.0F, percent));
  EXPECT_EQ(percent, 100);
  EXPECT_FALSE(DashboardProgress::toPercent(std::numeric_limits<float>::quiet_NaN(), percent));
}

TEST(DashboardProgress, TrustedCompletionDoesNotDependOnProgressOrSectionCache) {
  uint8_t percent = 7;
  EXPECT_TRUE(DashboardProgress::fromCompletedStats(true, true, percent));
  EXPECT_EQ(percent, 100);

  percent = 7;
  EXPECT_FALSE(DashboardProgress::fromCompletedStats(false, true, percent));
  EXPECT_EQ(percent, 7);
  EXPECT_FALSE(DashboardProgress::fromCompletedStats(true, false, percent));
  EXPECT_EQ(percent, 7);
}

TEST(DashboardProgress, CompactBarFillIsExactAndBounded) {
  EXPECT_EQ(DashboardProgress::fillWidth(200, 0), 0);
  EXPECT_EQ(DashboardProgress::fillWidth(200, 100), 196);
  EXPECT_EQ(DashboardProgress::fillWidth(200, 255), 196);
  EXPECT_EQ(DashboardProgress::fillWidth(4, 100), 0);
}

TEST(DashboardStatsPolicy, DistinguishesMissingZeroAndUnreadableBookStats) {
  DashboardStatsPolicyInput input;
  input.isEpub = true;
  input.epubVerified = true;
  input.localStatsTrusted = true;
  input.localStatsMissing = true;

  input.bookStatsTrusted = true;
  input.bookStatsMissing = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::NoData);

  input.bookStatsMissing = false;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Available);

  input.bookStatsTrusted = false;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, NonEpubFormatsAreExplicitlyNotTracked) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::NotTracked);

  input.isEpub = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, UsesOnlyACompleteVerifiedSyncedAggregate) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  input.hasSyncedDirectory = true;
  input.syncedScanComplete = true;
  input.validPeerCount = 2;

  DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_TRUE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Available);

  input.skippedPeerCount = 1;
  result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);

  input.skippedPeerCount = 0;
  input.syncedScanComplete = false;
  result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, UntrustedLocalStatsCannotBecomePlausibleSyncedZeros) {
  DashboardStatsPolicyInput input;
  input.hasSyncedDirectory = true;
  input.syncedScanComplete = true;
  input.validPeerCount = 2;

  const DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Unavailable);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, MissingLocalStatsAreKnownNoDataWithoutPeers) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  input.localStatsMissing = true;

  const DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_EQ(result.globalStats, DashboardMetricState::NoData);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::NoData);
}
