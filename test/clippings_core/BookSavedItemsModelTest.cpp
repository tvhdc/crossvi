#include <gtest/gtest.h>

#include <vector>

#include "activities/reader/BookSavedItemsModel.h"
#include "activities/reader/BookSavedItemsNavigation.h"

namespace {

BookmarkEntry epubBookmark(const uint16_t spine, const uint16_t page) {
  BookmarkEntry value;
  value.computedSpineIndex = spine;
  value.computedChapterProgress = page;
  return value;
}

TEST(BookSavedItemsModelTest, SortsByBookPositionAndKeepsBookmarkBeforeHighlightAtSamePosition) {
  std::vector<BookmarkEntry> bookmarks{epubBookmark(1, 2), epubBookmark(0, 1)};
  // A null store is still a valid bookmark-only projection.
  auto bookmarkOnly = BookSavedItemsModel::project(bookmarks, nullptr, BookSavedItemsModel::Tab::All);
  ASSERT_EQ(bookmarkOnly.size(), 2U);
  EXPECT_EQ(bookmarkOnly[0].sourceIndex, 1U);
  EXPECT_EQ(bookmarkOnly[1].sourceIndex, 0U);

  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 0;
  clipping.startPage = 1;
  EXPECT_LT(BookSavedItemsModel::highlightPosition(clipping), BookSavedItemsModel::bookmarkPosition(bookmarks[0]));

  std::vector<BookSavedItemsModel::ItemRef> tied{
      {BookSavedItemsModel::Kind::Highlight, 0, BookSavedItemsModel::highlightPosition(clipping)},
      {BookSavedItemsModel::Kind::Bookmark, 1, BookSavedItemsModel::bookmarkPosition(bookmarks[1])}};
  BookSavedItemsModel::sortItems(tied);
  EXPECT_EQ(tied[0].kind, BookSavedItemsModel::Kind::Bookmark);
  EXPECT_EQ(tied[1].kind, BookSavedItemsModel::Kind::Highlight);
}

TEST(BookSavedItemsModelTest, UsesRawTextAndFixedPageAnchors) {
  BookmarkEntry text;
  text.positionKind = BookmarkEntry::PositionKind::Text;
  text.byteOffset = 1234;
  BookmarkEntry fixed;
  fixed.positionKind = BookmarkEntry::PositionKind::FixedLayout;
  fixed.pageIndex = 27;

  EXPECT_EQ(BookSavedItemsModel::bookmarkPosition(text), 1234U);
  EXPECT_EQ(BookSavedItemsModel::bookmarkPosition(fixed), 27U);

  ClippingCodec::ClippingMetadata highlight;
  highlight.hasTextAnchor = true;
  highlight.textSourceStart = 1200;
  EXPECT_EQ(BookSavedItemsModel::highlightPosition(highlight), 1200U);
}

TEST(BookSavedItemsModelTest, AnchoredHighlightsSortBySpineBeforeTheirLocalTextOffset) {
  ClippingCodec::ClippingMetadata firstChapter;
  firstChapter.spineIndex = 1;
  firstChapter.hasTextAnchor = true;
  firstChapter.textSourceStart = 9000;

  ClippingCodec::ClippingMetadata secondChapter;
  secondChapter.spineIndex = 2;
  secondChapter.hasTextAnchor = true;
  secondChapter.textSourceStart = 10;

  auto laterInFirstChapter = firstChapter;
  laterInFirstChapter.textSourceStart = 9500;

  std::vector<BookSavedItemsModel::ItemRef> items{
      {BookSavedItemsModel::Kind::Highlight, 2, BookSavedItemsModel::highlightPosition(secondChapter)},
      {BookSavedItemsModel::Kind::Highlight, 1, BookSavedItemsModel::highlightPosition(laterInFirstChapter)},
      {BookSavedItemsModel::Kind::Highlight, 0, BookSavedItemsModel::highlightPosition(firstChapter)}};
  BookSavedItemsModel::sortItems(items);

  ASSERT_EQ(items.size(), 3U);
  EXPECT_EQ(items[0].sourceIndex, 0U);
  EXPECT_EQ(items[1].sourceIndex, 1U);
  EXPECT_EQ(items[2].sourceIndex, 2U);
  EXPECT_EQ(BookSavedItemsModel::highlightPosition(firstChapter), (uint64_t{1} << 32U) | 9000U);
}

TEST(BookSavedItemsModelTest, FiltersTabsWithoutCopyingSourceContent) {
  std::vector<BookmarkEntry> bookmarks{epubBookmark(0, 0), epubBookmark(0, 2)};
  auto all = BookSavedItemsModel::project(bookmarks, nullptr, BookSavedItemsModel::Tab::All);
  auto onlyBookmarks = BookSavedItemsModel::project(bookmarks, nullptr, BookSavedItemsModel::Tab::Bookmarks);
  auto onlyHighlights = BookSavedItemsModel::project(bookmarks, nullptr, BookSavedItemsModel::Tab::Highlights);
  EXPECT_EQ(all.size(), 2U);
  EXPECT_EQ(onlyBookmarks.size(), 2U);
  EXPECT_TRUE(onlyHighlights.empty());
}

TEST(BookSavedItemsNavigationTest, StartsAtTabAndAnEmptyListCannotLeaveIt) {
  BookSavedItemsNavigation::State state;

  EXPECT_TRUE(state.tabFocused);
  EXPECT_FALSE(BookSavedItemsNavigation::next(state, 0));
  EXPECT_FALSE(BookSavedItemsNavigation::previous(state, 0));
  EXPECT_TRUE(state.tabFocused);
  EXPECT_EQ(BookSavedItemsNavigation::back(state, 0), BookSavedItemsNavigation::BackAction::Exit);
}

TEST(BookSavedItemsNavigationTest, NextFromTheLastItemReturnsToTheTabThenReentersTheFirstItem) {
  BookSavedItemsNavigation::State state;

  EXPECT_TRUE(BookSavedItemsNavigation::next(state, 3));
  EXPECT_FALSE(state.tabFocused);
  EXPECT_EQ(state.selectedItem, 0);
  EXPECT_TRUE(BookSavedItemsNavigation::next(state, 3));
  EXPECT_EQ(state.selectedItem, 1);
  EXPECT_TRUE(BookSavedItemsNavigation::next(state, 3));
  EXPECT_EQ(state.selectedItem, 2);
  EXPECT_TRUE(BookSavedItemsNavigation::next(state, 3));
  EXPECT_TRUE(state.tabFocused);
  EXPECT_EQ(state.selectedItem, 2);
  EXPECT_TRUE(BookSavedItemsNavigation::next(state, 3));
  EXPECT_FALSE(state.tabFocused);
  EXPECT_EQ(state.selectedItem, 0);
}

TEST(BookSavedItemsNavigationTest, PreviousFromTheFirstItemReturnsToTheTab) {
  BookSavedItemsNavigation::State state;
  ASSERT_TRUE(BookSavedItemsNavigation::next(state, 2));

  EXPECT_TRUE(BookSavedItemsNavigation::previous(state, 2));
  EXPECT_TRUE(state.tabFocused);
  EXPECT_FALSE(BookSavedItemsNavigation::previous(state, 2));
}

TEST(BookSavedItemsNavigationTest, BackReturnsToTheTabBeforeItSignalsExit) {
  BookSavedItemsNavigation::State state{false, 2};

  EXPECT_EQ(BookSavedItemsNavigation::back(state, 3), BookSavedItemsNavigation::BackAction::FocusTab);
  EXPECT_TRUE(state.tabFocused);
  EXPECT_EQ(BookSavedItemsNavigation::back(state, 3), BookSavedItemsNavigation::BackAction::Exit);
}

TEST(BookSavedItemsNavigationTest, RestoreClampsAStaleItemSelectionAndEmptyListsToTheTab) {
  const auto clamped = BookSavedItemsNavigation::restore({false, 9}, 3);
  EXPECT_FALSE(clamped.tabFocused);
  EXPECT_EQ(clamped.selectedItem, 2);

  const auto empty = BookSavedItemsNavigation::restore({false, 9}, 0);
  EXPECT_TRUE(empty.tabFocused);
  EXPECT_EQ(empty.selectedItem, 0);
}

TEST(BookSavedItemsNavigationTest, TabsWrapWithinTheirConfiguredBoundsIncludingFixedLayout) {
  EXPECT_EQ(BookSavedItemsNavigation::nextTab(0, 3), 1);
  EXPECT_EQ(BookSavedItemsNavigation::nextTab(2, 3), 0);
  EXPECT_EQ(BookSavedItemsNavigation::previousTab(0, 3), 2);
  EXPECT_EQ(BookSavedItemsNavigation::previousTab(2, 3), 1);
  EXPECT_EQ(BookSavedItemsNavigation::nextTab(0, 1), 0);
  EXPECT_EQ(BookSavedItemsNavigation::previousTab(0, 1), 0);
}

}  // namespace
