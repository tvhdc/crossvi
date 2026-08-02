#include <gtest/gtest.h>

#include "Arduino.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr auto NEXT = MappedInputManager::Button::NavNext;
constexpr size_t NEXT_INDEX = static_cast<size_t>(NEXT);

class ButtonNavigatorTest : public testing::Test {
 protected:
  void SetUp() override {
    testMillis = 0;
    ButtonNavigator::setMappedInputManager(input_);
  }

  MappedInputManager input_;
};

TEST_F(ButtonNavigatorTest, IgnoresAReleaseWhosePressBelongedToThePreviousActivity) {
  ButtonNavigator navigator;
  input_.released[NEXT_INDEX] = true;
  int calls = 0;

  navigator.onNextRelease([&] { ++calls; });

  EXPECT_EQ(calls, 0);
}

TEST_F(ButtonNavigatorTest, AcceptsAReleaseAfterSeeingItsPress) {
  ButtonNavigator navigator;
  input_.pressed[NEXT_INDEX] = true;
  int calls = 0;
  navigator.onNextRelease([&] { ++calls; });

  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  navigator.onNextRelease([&] { ++calls; });

  EXPECT_EQ(calls, 1);
}

TEST_F(ButtonNavigatorTest, AcceptsPressAndReleaseInOneFrame) {
  ButtonNavigator navigator;
  input_.pressed[NEXT_INDEX] = true;
  input_.released[NEXT_INDEX] = true;
  int calls = 0;

  navigator.onNextRelease([&] { ++calls; });

  EXPECT_EQ(calls, 1);
}

TEST_F(ButtonNavigatorTest, ContinuousNavigationUsesTheSpecificButtonDuration) {
  ButtonNavigator navigator(0, 500);
  input_.pressed[NEXT_INDEX] = true;
  input_.held[NEXT_INDEX] = true;
  input_.heldTime[NEXT_INDEX] = 100;
  testMillis = 1000;
  int calls = 0;

  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 0);

  input_.clearEdges();
  input_.heldTime[NEXT_INDEX] = 501;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 1);
}
}  // namespace
