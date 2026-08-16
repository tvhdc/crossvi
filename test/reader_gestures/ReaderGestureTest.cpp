#include <gtest/gtest.h>

#include "ReaderUtils.h"
#include "TiltPageTurnPolicy.h"

CrossPointSettings testSettings;
HalTiltSensor halTiltSensor;

TEST(ReaderGesture, FiresConfirmHoldAtFiveHundredMillisecondsOnce) {
  ReaderUtils::HoldGestureState state;
  state.onPress();
  EXPECT_FALSE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS - 1, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_TRUE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_FALSE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS + 1, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_EQ(state.onRelease(), ReaderUtils::HoldRelease::Long);
}

TEST(ReaderGesture, FiresLongPageActionOnceAndConsumesRelease) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::CHAPTER_SKIP;

  input.pressed[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  input.held[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).prev);

  input.clearEdges();
  input.heldTime = ReaderUtils::SKIP_HOLD_MS;
  const auto held = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(held.prev);
  EXPECT_TRUE(held.longPress);
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).prev);

  input.held[static_cast<size_t>(MappedInputManager::Button::Left)] = false;
  input.released[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  EXPECT_TRUE(ReaderUtils::isLongPageTurnRelease(input, state));
  const auto released = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_FALSE(released.prev);
  EXPECT_FALSE(released.next);
}

TEST(ReaderGesture, UsesHeldTimeOfTheActiveFrontButton) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::CHAPTER_SKIP;

  const auto left = static_cast<size_t>(MappedInputManager::Button::Left);
  const auto pageBack = static_cast<size_t>(MappedInputManager::Button::PageBack);
  input.pressed[left] = true;
  input.held[left] = true;
  input.heldTimes[pageBack] = ReaderUtils::SKIP_HOLD_MS + 100;
  input.heldTimes[left] = ReaderUtils::SKIP_HOLD_MS - 1;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).prev);

  input.clearEdges();
  input.heldTimes[left] = ReaderUtils::SKIP_HOLD_MS;
  const auto held = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(held.prev);
  EXPECT_TRUE(held.longPress);

  input = {};
  state.reset();
  const auto right = static_cast<size_t>(MappedInputManager::Button::Right);
  const auto pageForward = static_cast<size_t>(MappedInputManager::Button::PageForward);
  input.pressed[right] = true;
  input.held[right] = true;
  input.heldTimes[pageForward] = ReaderUtils::SKIP_HOLD_MS + 100;
  input.heldTimes[right] = ReaderUtils::SKIP_HOLD_MS - 1;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).next);

  input.clearEdges();
  input.heldTimes[right] = ReaderUtils::SKIP_HOLD_MS;
  const auto nextHeld = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(nextHeld.next);
  EXPECT_TRUE(nextHeld.longPress);
}

TEST(ReaderGesture, DefersEnabledShortPageTurnUntilRelease) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::ORIENTATION_CHANGE;

  input.pressed[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  input.held[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).next);

  input.clearEdges();
  input.held[static_cast<size_t>(MappedInputManager::Button::Right)] = false;
  input.released[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  const auto released = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(released.next);
  EXPECT_FALSE(released.longPress);
}

TEST(ReaderGesture, UsesThirtySecondsOnlyWhenNoPreviousAutoTurnIntervalExists) {
  EXPECT_EQ(ReaderUtils::autoPageTurnShortcutSeconds(0), 30);
  EXPECT_EQ(ReaderUtils::autoPageTurnShortcutSeconds(45), 45);
}

TEST(ReaderRendering, SkipsEveryGrayscaleOperationWhenDriverDoesNotSupportIt) {
  GfxRenderer renderer;
  renderer.stripGrayscaleSupported = false;
  int renderCalls = 0;

  ReaderUtils::renderAntiAliased(renderer, [&renderCalls]() { renderCalls++; });

  EXPECT_EQ(renderCalls, 0);
  EXPECT_EQ(renderer.storeBwBufferCalls, 0);
  EXPECT_EQ(renderer.clearScreenCalls, 0);
  EXPECT_EQ(renderer.setRenderModeCalls, 0);
  EXPECT_EQ(renderer.copyLsbCalls, 0);
  EXPECT_EQ(renderer.copyMsbCalls, 0);
  EXPECT_EQ(renderer.displayGrayCalls, 0);
  EXPECT_EQ(renderer.restoreBwBufferCalls, 0);
}

TEST(ReaderRendering, UsesBoundedStripPathForSupportedDrivers) {
  GfxRenderer renderer;
  int renderCalls = 0;

  ReaderUtils::renderAntiAliased(renderer, [&renderCalls]() { renderCalls++; });

  EXPECT_EQ(renderCalls, 2);
  EXPECT_EQ(renderer.storeBwBufferCalls, 0);
  EXPECT_EQ(renderer.clearScreenCalls, 2);
  EXPECT_EQ(renderer.setRenderModeCalls, 3);
  EXPECT_EQ(renderer.copyLsbCalls, 0);
  EXPECT_EQ(renderer.copyMsbCalls, 0);
  EXPECT_EQ(renderer.beginStripCalls, 2);
  EXPECT_EQ(renderer.endStripCalls, 2);
  EXPECT_EQ(renderer.writeLsbStripCalls, 1);
  EXPECT_EQ(renderer.writeMsbStripCalls, 1);
  EXPECT_EQ(renderer.displayGrayCalls, 1);
  EXPECT_EQ(renderer.restoreBwBufferCalls, 0);
  EXPECT_EQ(renderer.cleanupGrayscaleCalls, 1);
}

TEST(ReaderRendering, ThirteenKiBStripBudgetCutsX3AndX4ToEightContentPasses) {
  const auto contentPasses = [](const int widthBytes, const int height) {
    const int rows = ReaderUtils::grayscaleStripRows(widthBytes, height);
    EXPECT_GT(rows, 0);
    EXPECT_LE(static_cast<size_t>(widthBytes) * rows, ReaderUtils::GRAYSCALE_STRIP_SCRATCH_BYTES);
    return 2 * ((height + rows - 1) / rows);
  };

  EXPECT_EQ(contentPasses(800 / 8, 480), 8);
  EXPECT_EQ(contentPasses(792 / 8, 528), 8);
}

TEST(ReaderRendering, ReusesCallerOwnedGrayscaleScratchAcrossPages) {
  GfxRenderer renderer;
  std::unique_ptr<uint8_t[]> scratch;
  size_t capacity = 0;

  ReaderUtils::renderAntiAliased(renderer, scratch, capacity, [] {});
  uint8_t* const firstAllocation = scratch.get();
  ASSERT_NE(firstAllocation, nullptr);
  ReaderUtils::renderAntiAliased(renderer, scratch, capacity, [] {});

  EXPECT_EQ(scratch.get(), firstAllocation);
  EXPECT_LE(capacity, ReaderUtils::GRAYSCALE_STRIP_SCRATCH_BYTES);
}

TEST(ReaderGesture, ConsumesTheReleaseThatOpenedAReplacementActivity) {
  bool armed = true;
  EXPECT_TRUE(ReaderUtils::consumeInitialRelease(armed, false, true));
  EXPECT_TRUE(armed);
  EXPECT_TRUE(ReaderUtils::consumeInitialRelease(armed, true, false));
  EXPECT_FALSE(armed);
  EXPECT_FALSE(ReaderUtils::consumeInitialRelease(armed, false, false));
}

TEST(ReaderGesture, KeepsBackNavigationBoundaryAtOneSecond) {
  MappedInputManager input;
  ActivityManager activities;
  struct NavigationTrace {
    bool prepared = false;
    bool wentHomeAfterPreparation = false;
  } trace;
  const ReaderUtils::BackNavCallback goHome{&trace, [](void* ctx) {
                                              auto& state = *static_cast<NavigationTrace*>(ctx);
                                              state.wentHomeAfterPreparation = state.prepared;
                                            }};
  const ReaderUtils::BackNavCallback prepare{&trace,
                                             [](void* ctx) { static_cast<NavigationTrace*>(ctx)->prepared = true; }};
  input.held[static_cast<size_t>(MappedInputManager::Button::Back)] = true;
  input.heldTime = ReaderUtils::GO_BACK_OR_HOME_MS;
  EXPECT_TRUE(ReaderUtils::handleBackNavigation(input, activities, "/book.epub", goHome, prepare));
  EXPECT_TRUE(trace.prepared);
  EXPECT_TRUE(activities.openedFileBrowser);
  EXPECT_FALSE(trace.wentHomeAfterPreparation);

  trace.prepared = false;
  input.held.fill(false);
  input.released[static_cast<size_t>(MappedInputManager::Button::Back)] = true;
  input.heldTime = ReaderUtils::GO_BACK_OR_HOME_MS - 1;
  EXPECT_TRUE(ReaderUtils::handleBackNavigation(input, activities, "/book.epub", goHome, prepare));
  EXPECT_TRUE(trace.prepared);
  EXPECT_TRUE(trace.wentHomeAfterPreparation);
}

TEST(ReaderGesture, DefaultPhysicalDirectionsTurnExpectedPages) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::OFF;

  input.pressed[static_cast<size_t>(MappedInputManager::Button::PageBack)] = true;
  EXPECT_TRUE(ReaderUtils::detectPageTurnGesture(input, state).prev);
  input.clearEdges();
  input.pressed[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  EXPECT_TRUE(ReaderUtils::detectPageTurnGesture(input, state).prev);
  input.clearEdges();
  input.pressed[static_cast<size_t>(MappedInputManager::Button::PageForward)] = true;
  EXPECT_TRUE(ReaderUtils::detectPageTurnGesture(input, state).next);
  input.clearEdges();
  input.pressed[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  EXPECT_TRUE(ReaderUtils::detectPageTurnGesture(input, state).next);
}

TEST(ReaderGesture, TiltTurnsBothDirectionsWhenEnabled) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::OFF;
  testSettings.tiltPageTurn = 1;

  halTiltSensor.forward = true;
  EXPECT_TRUE(ReaderUtils::detectPageTurnGesture(input, state).next);
  halTiltSensor.back = true;
  const auto opposite = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(opposite.prev);
  EXPECT_FALSE(opposite.next);
}

TEST(TiltPageTurnPolicy, DecodesConfiguredBigEndianSamples) {
  EXPECT_EQ(TiltPageTurnPolicy::decodeBigEndian(0x12, 0x34), 0x1234);
  EXPECT_EQ(TiltPageTurnPolicy::decodeBigEndian(0xFF, 0x00), -256);
}

TEST(TiltPageTurnPolicy, MapsRightAndLeftModesAcrossOrientation) {
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 0, 1), 0.5f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 0, 2), -0.5f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 1, 1), -0.25f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 1, 2), 0.25f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 2, 1), -0.5f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 2, 2), 0.5f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 3, 1), 0.25f);
  EXPECT_FLOAT_EQ(TiltPageTurnPolicy::selectedAxis(0.5f, 0.25f, 3, 2), -0.25f);
  EXPECT_TRUE(TiltPageTurnPolicy::shouldTurnNext(0.51f, TiltPageTurnPolicy::TRIGGER_THRESHOLD_G));
  EXPECT_FALSE(TiltPageTurnPolicy::shouldTurnNext(0.49f, TiltPageTurnPolicy::TRIGGER_THRESHOLD_G));
  EXPECT_FALSE(TiltPageTurnPolicy::shouldTurnNext(-0.51f, TiltPageTurnPolicy::TRIGGER_THRESHOLD_G));
}

TEST(TiltPageTurnPolicy, KeepsExistingDirectionSelectedAsReversed) {
  EXPECT_EQ(TiltPageTurnPolicy::settingOptionForMode(0), 0);
  EXPECT_EQ(TiltPageTurnPolicy::settingOptionForMode(1), 2);
  EXPECT_EQ(TiltPageTurnPolicy::settingOptionForMode(2), 1);
  EXPECT_EQ(TiltPageTurnPolicy::modeForSettingOption(0), 0);
  EXPECT_EQ(TiltPageTurnPolicy::modeForSettingOption(1), 2);
  EXPECT_EQ(TiltPageTurnPolicy::modeForSettingOption(2), 1);
  EXPECT_EQ(TiltPageTurnPolicy::modeForSettingOption(99), 0);
}

TEST(TiltPageTurnPolicy, RequiresNeutralAndTwoDeliberateSamples) {
  TiltPageTurnPolicy::GestureState state;
  using Direction = TiltPageTurnPolicy::Direction;

  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.70f, 0, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 50, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 100, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.60f, 150, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.60f, 200, true), Direction::Forward);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.60f, 1000, true), Direction::None);
}

TEST(TiltPageTurnPolicy, RenderBusyDiscardsGestureUntilNeutralAgain) {
  TiltPageTurnPolicy::GestureState state;
  using Direction = TiltPageTurnPolicy::Direction;

  TiltPageTurnPolicy::updateGesture(state, 0.00f, 0, true);
  TiltPageTurnPolicy::updateGesture(state, 0.00f, 50, true);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 100, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 150, false), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 800, true), Direction::None);
  TiltPageTurnPolicy::updateGesture(state, 0.00f, 850, true);
  TiltPageTurnPolicy::updateGesture(state, 0.00f, 900, true);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 950, true), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 1000, true), Direction::Back);
}

TEST(TiltPageTurnPolicy, WaitsForCompletedPageRenderBeforeRearming) {
  TiltPageTurnPolicy::GestureState state;
  using Direction = TiltPageTurnPolicy::Direction;

  TiltPageTurnPolicy::updateGesture(state, 0.00f, 0, true, 10);
  TiltPageTurnPolicy::updateGesture(state, 0.00f, 50, true, 10);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.70f, 100, true, 10), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.70f, 150, true, 10), Direction::Forward);

  // Render was requested but has not completed: even a complete neutral/new
  // gesture sequence must remain blocked.
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 800, true, 10), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 850, true, 10), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 900, true, 10), Direction::None);

  // Generation 11 acknowledges the displayed page. The device must then be
  // level for two new samples before another deliberate tilt is accepted.
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 950, true, 11), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 1000, true, 11), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, 0.00f, 1050, true, 11), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 1100, true, 11), Direction::None);
  EXPECT_EQ(TiltPageTurnPolicy::updateGesture(state, -0.70f, 1150, true, 11), Direction::Back);
}
