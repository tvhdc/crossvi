#include <Preferences.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "HalGPIO.h"
#include "XteinkDetect.h"

namespace {
constexpr uint8_t POWER_MASK = 1U << HalGPIO::BTN_POWER;

class HalGPIOTest : public testing::Test {
 protected:
  void SetUp() override {
    ArduinoFake::reset();
    InputManager::reset();
    PreferencesFake::reset();
    freeink::XteinkDetectFake::reset();
    WireFake::reset();
    EspFake::wakeupCause = ESP_SLEEP_WAKEUP_UNDEFINED;
    EspFake::resetReason = ESP_RST_POWERON;
  }
};

TEST_F(HalGPIOTest, WakeVerificationDoesNotWaitForAnotherPress) {
  HalGPIO subject;
  ArduinoFake::delayHook = [] { InputManager::rawState = POWER_MASK; };

  EXPECT_FALSE(subject.verifyPowerButtonWakeup(400, false));
  EXPECT_EQ(ArduinoFake::now, 0U);
}

TEST_F(HalGPIOTest, WakeVerificationAcceptsOnlyTheCurrentContinuousHold) {
  HalGPIO subject;
  InputManager::rawState = POWER_MASK;

  EXPECT_TRUE(subject.verifyPowerButtonWakeup(20, false));
  EXPECT_GE(ArduinoFake::now, 20U);
  EXPECT_LT(ArduinoFake::now, 40U);
}

TEST_F(HalGPIOTest, WakeVerificationRejectsReleaseBeforeThreshold) {
  HalGPIO subject;
  InputManager::rawState = POWER_MASK;
  ArduinoFake::delayHook = [] {
    if (ArduinoFake::now >= 10) {
      InputManager::rawState = 0;
    }
  };

  EXPECT_FALSE(subject.verifyPowerButtonWakeup(20, false));
}

TEST_F(HalGPIOTest, WakeVerificationDoesNotReuseStaleDurationForASecondPress) {
  HalGPIO subject;
  InputManager::rawState = POWER_MASK;
  subject.update();

  // The first release and second press both happen while setup is not polling.
  InputManager::rawState = 0;
  ArduinoFake::now = 1000;
  InputManager::rawState = POWER_MASK;
  ArduinoFake::delayHook = [] {
    if (ArduinoFake::now >= 1010) InputManager::rawState = 0;
  };

  EXPECT_FALSE(subject.verifyPowerButtonWakeup(20, false));
}

TEST_F(HalGPIOTest, WakeVerificationMeasuresTheFullRawHoldFromEntry) {
  HalGPIO subject;
  InputManager::rawState = POWER_MASK;
  subject.update();
  ArduinoFake::now = 1000;

  EXPECT_TRUE(subject.verifyPowerButtonWakeup(20, false));
  EXPECT_GE(ArduinoFake::now, 1020U);
}

TEST_F(HalGPIOTest, WakeVerificationKeepsShortPressFastPath) {
  HalGPIO subject;
  EXPECT_TRUE(subject.verifyPowerButtonWakeup(400, true));
  EXPECT_EQ(ArduinoFake::now, 0U);
}

TEST_F(HalGPIOTest, WakeHoldDurationSurvivesMillisWrap) {
  HalGPIO subject;
  ArduinoFake::now = std::numeric_limits<uint32_t>::max() - 5U;
  InputManager::rawState = POWER_MASK;
  subject.update();

  EXPECT_TRUE(subject.verifyPowerButtonWakeup(20, false));
}

TEST_F(HalGPIOTest, TracksOverlappingButtonDurationsIndependently) {
  HalGPIO subject;
  constexpr uint8_t RIGHT_MASK = 1U << HalGPIO::BTN_RIGHT;
  constexpr uint8_t CONFIRM_MASK = 1U << HalGPIO::BTN_CONFIRM;

  InputManager::rawState = RIGHT_MASK;
  subject.update();
  ArduinoFake::now = 600;
  InputManager::rawState = RIGHT_MASK | CONFIRM_MASK;
  subject.update();

  EXPECT_EQ(subject.getHeldTime(HalGPIO::BTN_RIGHT), 600U);
  EXPECT_EQ(subject.getHeldTime(HalGPIO::BTN_CONFIRM), 0U);

  ArduinoFake::now = 700;
  InputManager::rawState = RIGHT_MASK;
  subject.update();
  EXPECT_EQ(subject.getHeldTime(HalGPIO::BTN_RIGHT), 700U);
  EXPECT_EQ(subject.getHeldTime(HalGPIO::BTN_CONFIRM), 100U);
}

TEST_F(HalGPIOTest, X3UsbPollingIsLimitedAndEdgesFollowSuccessfulSamples) {
  HalGPIO subject;
  PreferencesFake::cachedDevice = 2;
  subject.begin();
  ArduinoFake::now = 100;
  WireFake::currentMa = 25;

  subject.update();
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_FALSE(subject.wasUsbStateChanged());
  EXPECT_EQ(WireFake::transactionCount, 1U);

  ArduinoFake::now = 599;
  WireFake::currentMa = -10;
  subject.update();
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_EQ(WireFake::transactionCount, 1U);

  ArduinoFake::now = 600;
  subject.update();
  EXPECT_FALSE(subject.isUsbConnected());
  EXPECT_TRUE(subject.wasUsbStateChanged());
  EXPECT_EQ(WireFake::transactionCount, 2U);
}

TEST_F(HalGPIOTest, X3DisplayDetectionUsesLiveProbeInsteadOfStaleCache) {
  PreferencesFake::cachedDevice = 2;
  PreferencesFake::epdCached = 1;
  freeink::XteinkDetectFake::verdict = freeink::X3DisplayVerdict::Uc8279Confirmed;

  HalGPIO subject;
  subject.begin();

  EXPECT_EQ(freeink::XteinkDetectFake::callCount, 1U);
}

TEST_F(HalGPIOTest, ExplicitX3DisplayOverrideStillSkipsLiveProbe) {
  PreferencesFake::cachedDevice = 2;
  PreferencesFake::epdOverride = 1;
  freeink::XteinkDetectFake::verdict = freeink::X3DisplayVerdict::Uc8279Confirmed;

  HalGPIO subject;
  subject.begin();

  EXPECT_EQ(freeink::XteinkDetectFake::callCount, 0U);
}

TEST_F(HalGPIOTest, X3UsbReadFailureKeepsLastKnownStateAndIsRateLimited) {
  HalGPIO subject;
  PreferencesFake::cachedDevice = 2;
  subject.begin();
  WireFake::currentMa = 25;
  subject.update();

  ArduinoFake::now = 500;
  WireFake::readsSucceed = false;
  subject.update();
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_FALSE(subject.wasUsbStateChanged());
  EXPECT_EQ(WireFake::transactionCount, 2U);

  ArduinoFake::now = 999;
  subject.update();
  EXPECT_EQ(WireFake::transactionCount, 2U);

  ArduinoFake::now = 1000;
  WireFake::readsSucceed = true;
  WireFake::currentMa = -10;
  subject.update();
  EXPECT_FALSE(subject.isUsbConnected());
  EXPECT_TRUE(subject.wasUsbStateChanged());
  EXPECT_EQ(WireFake::transactionCount, 3U);
}

TEST_F(HalGPIOTest, X4SamplesTheDirectGpioOnEveryUpdate) {
  HalGPIO subject;
  PreferencesFake::cachedDevice = 1;
  subject.begin();
  ArduinoFake::digitalValue = HIGH;

  subject.update();
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_FALSE(subject.wasUsbStateChanged());
  subject.update();
  EXPECT_EQ(ArduinoFake::digitalReadCount, 2U);

  ArduinoFake::digitalValue = LOW;
  subject.update();
  EXPECT_FALSE(subject.isUsbConnected());
  EXPECT_TRUE(subject.wasUsbStateChanged());
  EXPECT_EQ(ArduinoFake::digitalReadCount, 3U);
}

TEST_F(HalGPIOTest, WakeReasonForcesFreshUsbReadAndSeedsCache) {
  HalGPIO subject;
  PreferencesFake::cachedDevice = 2;
  subject.begin();
  WireFake::currentMa = 25;

  EXPECT_EQ(subject.getWakeupReason(), HalGPIO::WakeupReason::AfterUSBPower);
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_EQ(WireFake::transactionCount, 1U);

  WireFake::currentMa = -10;
  EXPECT_EQ(subject.getWakeupReason(), HalGPIO::WakeupReason::PowerButton);
  EXPECT_FALSE(subject.isUsbConnected());
  EXPECT_EQ(WireFake::transactionCount, 2U);
}

TEST_F(HalGPIOTest, FailedFreshWakeReadUsesLastKnownUsbState) {
  HalGPIO subject;
  PreferencesFake::cachedDevice = 2;
  subject.begin();
  WireFake::currentMa = 25;
  EXPECT_EQ(subject.getWakeupReason(), HalGPIO::WakeupReason::AfterUSBPower);

  WireFake::readsSucceed = false;
  EXPECT_EQ(subject.getWakeupReason(), HalGPIO::WakeupReason::AfterUSBPower);
  EXPECT_TRUE(subject.isUsbConnected());
  EXPECT_EQ(WireFake::transactionCount, 2U);
}
}  // namespace
