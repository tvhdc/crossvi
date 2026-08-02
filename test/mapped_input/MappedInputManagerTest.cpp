#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"

CrossPointSettings CrossPointSettings::instance;

namespace {
class MappedInputManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    SETTINGS.frontButtonBack = HalGPIO::BTN_BACK;
    SETTINGS.frontButtonConfirm = HalGPIO::BTN_CONFIRM;
    SETTINGS.frontButtonLeft = HalGPIO::BTN_LEFT;
    SETTINGS.frontButtonRight = HalGPIO::BTN_RIGHT;
    SETTINGS.sideButtonLayout = CrossPointSettings::PREV_NEXT;
    SETTINGS.frontButtonFollowOrientation = false;
  }

  HalGPIO gpio_;
  GfxRenderer renderer_;
  MappedInputManager input_{gpio_, renderer_};
};

TEST_F(MappedInputManagerTest, UsesTheRemappedPhysicalButtonDuration) {
  SETTINGS.frontButtonConfirm = HalGPIO::BTN_LEFT;
  gpio_.held[HalGPIO::BTN_LEFT] = true;
  gpio_.heldTime[HalGPIO::BTN_LEFT] = 650;
  gpio_.heldTime[HalGPIO::BTN_CONFIRM] = 900;

  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::Confirm), 650U);
}

TEST_F(MappedInputManagerTest, UsesTheConfiguredSideButtonForPageHolds) {
  SETTINGS.sideButtonLayout = CrossPointSettings::NEXT_PREV;
  gpio_.heldTime[HalGPIO::BTN_UP] = 700;
  gpio_.heldTime[HalGPIO::BTN_DOWN] = 300;

  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::PageForward), 700U);
  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::PageBack), 300U);
}

TEST_F(MappedInputManagerTest, FollowsTheLiveOrientationForLogicalNavigation) {
  SETTINGS.frontButtonFollowOrientation = true;
  gpio_.held[HalGPIO::BTN_DOWN] = true;
  gpio_.heldTime[HalGPIO::BTN_DOWN] = 400;
  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::NavNext), 400U);

  gpio_.held.fill(false);
  renderer_.orientation = GfxRenderer::PortraitInverted;
  gpio_.held[HalGPIO::BTN_UP] = true;
  gpio_.heldTime[HalGPIO::BTN_UP] = 800;
  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::NavNext), 800U);
}

TEST_F(MappedInputManagerTest, IgnoresAStaleDurationFromTheOtherNavigationCandidate) {
  gpio_.heldTime[HalGPIO::BTN_DOWN] = 900;
  gpio_.held[HalGPIO::BTN_RIGHT] = true;
  gpio_.heldTime[HalGPIO::BTN_RIGHT] = 120;

  EXPECT_EQ(input_.getHeldTime(MappedInputManager::Button::NavNext), 120U);
}
}  // namespace
