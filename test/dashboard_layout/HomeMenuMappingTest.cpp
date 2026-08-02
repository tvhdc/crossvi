#include <gtest/gtest.h>

#include "activities/home/HomeMenuMapping.h"

TEST(HomeMenuMapping, ReadingStatsSitsImmediatelyAfterRecentBooks) {
  for (const bool hasReadingStats : {false, true}) {
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_BROWSER, false, hasReadingStats), 0);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::RECENTS, false, hasReadingStats), 1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, false, hasReadingStats), hasReadingStats ? 2 : -1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SAVED_ITEMS, false, hasReadingStats), hasReadingStats ? 3 : 2);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_TRANSFER, false, hasReadingStats), hasReadingStats ? 4 : 3);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SETTINGS_MENU, false, hasReadingStats), hasReadingStats ? 5 : 4);

    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_BROWSER, true, hasReadingStats), 0);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::RECENTS, true, hasReadingStats), 1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, true, hasReadingStats), hasReadingStats ? 2 : -1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SAVED_ITEMS, true, hasReadingStats), hasReadingStats ? 3 : 2);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::OPDS_BROWSER, true, hasReadingStats), hasReadingStats ? 4 : 3);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_TRANSFER, true, hasReadingStats), hasReadingStats ? 5 : 4);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SETTINGS_MENU, true, hasReadingStats), hasReadingStats ? 6 : 5);
  }
}

TEST(HomeMenuMapping, ReadingStatsIsPresentOnlyWhenAvailable) {
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, false, false), -1);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, true, false), -1);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, false, true), 2);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, true, true), 2);
  EXPECT_EQ(HomeMenuMapping::itemCount(false, true), 6);
  EXPECT_EQ(HomeMenuMapping::itemCount(true, true), 7);
}

TEST(HomeMenuMapping, IndexRoundTripsAcrossOptionalStates) {
  for (const int recentBookCount : {0, 1, 3, 5}) {
    for (const bool hasOpds : {false, true}) {
      for (const bool hasReadingStats : {false, true}) {
        const int menuCount = HomeMenuMapping::itemCount(hasOpds, hasReadingStats);
        EXPECT_EQ(HomeMenuMapping::selectionCount(recentBookCount, hasOpds, hasReadingStats),
                  recentBookCount + menuCount);
        for (int menuIndex = 0; menuIndex < menuCount; ++menuIndex) {
          const HomeMenuItem action = HomeMenuMapping::actionAt(menuIndex, hasOpds, hasReadingStats);
          EXPECT_NE(action, HomeMenuItem::NONE);
          EXPECT_EQ(HomeMenuMapping::indexOf(action, hasOpds, hasReadingStats), menuIndex);
          EXPECT_EQ(HomeMenuMapping::selectorIndexOf(action, recentBookCount, hasOpds, hasReadingStats),
                    recentBookCount + menuIndex);
        }
        EXPECT_EQ(HomeMenuMapping::actionAt(-1, hasOpds, hasReadingStats), HomeMenuItem::NONE);
        EXPECT_EQ(HomeMenuMapping::actionAt(menuCount, hasOpds, hasReadingStats), HomeMenuItem::NONE);
      }
    }
  }
}

TEST(HomeMenuMapping, CrossViGridNavigationFollowsPhysicalDirections) {
  using Direction = HomeMenuMapping::GridDirection;
  constexpr int bookCount = 1;
  constexpr int menuCount = 6;
  constexpr int browse = bookCount;

  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(browse, bookCount, menuCount, Direction::Right), browse + 1);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(browse, bookCount, menuCount, Direction::Down), browse + 2);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(browse + 3, bookCount, menuCount, Direction::Left), browse + 2);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(browse + 4, bookCount, menuCount, Direction::Up), browse + 2);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(browse, bookCount, menuCount, Direction::Up), 0);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(0, bookCount, menuCount, Direction::Down), browse);
}

TEST(HomeMenuMapping, CrossViGridDoesNotWrapAcrossMissingCells) {
  using Direction = HomeMenuMapping::GridDirection;
  constexpr int menuCount = 5;

  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(4, 0, menuCount, Direction::Right), 4);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(3, 0, menuCount, Direction::Down), 3);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(4, 0, menuCount, Direction::Down), 4);
  EXPECT_EQ(HomeMenuMapping::moveCrossViGrid(0, 0, menuCount, Direction::Left), 0);
}
