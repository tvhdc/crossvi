#include <gtest/gtest.h>

#include <cstdint>

#include "RenderGeneration.h"
#include "util/PressReleaseLatch.h"

TEST(PressReleaseLatchTest, IgnoresReleaseNotOwnedByCurrentActivity) {
  PressReleaseLatch latch;
  EXPECT_FALSE(latch.update(false, true));
}

TEST(PressReleaseLatchTest, AcceptsOwnedReleaseAndConsumesItOnce) {
  PressReleaseLatch latch;
  EXPECT_FALSE(latch.update(true, false));
  EXPECT_TRUE(latch.update(false, true));
  EXPECT_FALSE(latch.update(false, true));
}

TEST(PressReleaseLatchTest, AcceptsSyntheticPressAndReleaseInSameFrame) {
  PressReleaseLatch latch;
  EXPECT_TRUE(latch.update(true, true));
}

TEST(RenderGenerationTest, OlderRenderDoesNotSatisfyNewWaiter) { EXPECT_FALSE(RenderGeneration::reached(7, 8)); }

TEST(RenderGenerationTest, CoalescedNewerRenderSatisfiesWaiter) { EXPECT_TRUE(RenderGeneration::reached(10, 8)); }

TEST(RenderGenerationTest, ComparisonSurvivesCounterWrap) {
  EXPECT_FALSE(RenderGeneration::reached(UINT32_MAX, 0));
  EXPECT_TRUE(RenderGeneration::reached(0, UINT32_MAX));
  EXPECT_TRUE(RenderGeneration::reached(0, 0));
}
