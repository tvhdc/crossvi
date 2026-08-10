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
  input_.heldTime[NEXT_INDEX] = 500;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 1);
}

TEST_F(ButtonNavigatorTest, ContinuousNavigationRepeatsAtTheConfiguredInterval) {
  ButtonNavigator navigator(500, 500);
  input_.pressed[NEXT_INDEX] = true;
  input_.held[NEXT_INDEX] = true;
  int calls = 0;

  navigator.onNextContinuous([&] { ++calls; });
  input_.clearEdges();

  input_.heldTime[NEXT_INDEX] = 500;
  testMillis = 500;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 1);

  input_.heldTime[NEXT_INDEX] = 999;
  testMillis = 999;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 1);

  input_.heldTime[NEXT_INDEX] = 1000;
  testMillis = 1000;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 2);
}

TEST_F(ButtonNavigatorTest, CoalescesBouncedPressCyclesIntoOneMove) {
  ButtonNavigator navigator;
  int calls = 0;

  testMillis = 1000;
  input_.pressed[NEXT_INDEX] = true;
  navigator.onNext([&] { ++calls; });

  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 1040;
  navigator.onNext([&] { ++calls; });

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1080;
  navigator.onNext([&] { ++calls; });

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1120;
  navigator.onNext([&] { ++calls; });

  EXPECT_EQ(calls, 1);

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1150;
  navigator.onNext([&] { ++calls; });
  EXPECT_EQ(calls, 2);
}

TEST_F(ButtonNavigatorTest, CoalescesBouncedReleaseCyclesIntoOneMove) {
  ButtonNavigator navigator;
  int calls = 0;

  testMillis = 1000;
  input_.pressed[NEXT_INDEX] = true;
  navigator.onNextRelease([&] { ++calls; });
  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 1040;
  navigator.onNextRelease([&] { ++calls; });

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1080;
  navigator.onNextRelease([&] { ++calls; });
  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 1120;
  navigator.onNextRelease([&] { ++calls; });

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1140;
  navigator.onNextRelease([&] { ++calls; });
  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 1160;
  navigator.onNextRelease([&] { ++calls; });

  EXPECT_EQ(calls, 1);

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 1190;
  navigator.onNextRelease([&] { ++calls; });
  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 1230;
  navigator.onNextRelease([&] { ++calls; });
  EXPECT_EQ(calls, 2);
}

TEST_F(ButtonNavigatorTest, ReleaseBounceAfterAContinuousMoveDoesNotAddAnotherMove) {
  ButtonNavigator navigator(500, 500);
  int calls = 0;

  input_.pressed[NEXT_INDEX] = true;
  input_.held[NEXT_INDEX] = true;
  navigator.onNextRelease([&] { ++calls; });

  input_.clearEdges();
  input_.heldTime[NEXT_INDEX] = 600;
  testMillis = 600;
  navigator.onNextContinuous([&] { ++calls; });
  EXPECT_EQ(calls, 1);

  input_.held[NEXT_INDEX] = false;
  input_.released[NEXT_INDEX] = true;
  testMillis = 620;
  navigator.onNextRelease([&] { ++calls; });

  input_.clearEdges();
  input_.pressed[NEXT_INDEX] = true;
  testMillis = 650;
  navigator.onNextRelease([&] { ++calls; });
  input_.clearEdges();
  input_.released[NEXT_INDEX] = true;
  testMillis = 680;
  navigator.onNextRelease([&] { ++calls; });

  EXPECT_EQ(calls, 1);
}
}  // namespace
