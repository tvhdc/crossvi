#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc)) {
    return false;
  }
  if (soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    return false;
  }
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;

  return tensDigit <= 5 && onesDigit <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3
constexpr unsigned long X3_USB_POLL_INTERVAL_MS = 500;

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run active X3 fingerprint probe and persist result.
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  delay(2);
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

// Newer X3 production units use a UC8279d panel controller on the same board,
// glass, and pins. Probe it before SPI owns the EPD pins. Only an explicit
// recovery override is trusted: a cached/factory value can describe a different
// panel after a full flash and must not override the live display-bus probe.
constexpr char NVS_KEY_EPD_OVERRIDE[] = "epd_ovr";  // 0=auto, 1=uc8253, 2=uc8279

bool detectX3DisplayIsUc8279() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue != NvsDeviceValue::Unknown) {
    LOG_INF("HW", "EPD controller override active: %s", overrideValue == NvsDeviceValue::X3 ? "UC8279" : "UC8253");
    return overrideValue == NvsDeviceValue::X3;
  }

  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  const freeink::X3DisplayVerdict verdict = freeink::detectX3DisplayController(ver, &flg);
  LOG_INF("HW", "EPD probe: ver=%02X %02X %02X %02X %02X flg=%02X verdict=%u", ver[0], ver[1], ver[2], ver[3], ver[4],
          flg, static_cast<unsigned>(verdict));
  if (verdict == freeink::X3DisplayVerdict::Uc8279Confirmed) {
    return true;
  }
  // An inconclusive probe conservatively uses the established UC8253 driver.
  // The next boot probes again instead of persisting a possibly wrong result.
  return false;
}

}  // namespace

void HalGPIO::begin() {
  _deviceType = detectDeviceTypeWithFingerprint();
  const bool x3IsUc8279 = deviceIsX3() && detectX3DisplayIsUc8279();
  BoardConfig::selectDevice(!deviceIsX3() ? BoardConfig::Board::XteinkX4
                            : x3IsUc8279  ? BoardConfig::Board::XteinkX3Uc8279
                                          : BoardConfig::Board::XteinkX3);

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
  inputMgr.begin();
}

void HalGPIO::update() {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const bool debounceWasPending = inputMgr.isDebouncePending();
#endif
  inputMgr.update();
  usbStateChanged = false;

  const unsigned long now = millis();
  for (size_t button = 0; button < BUTTON_COUNT; ++button) {
    if (inputMgr.wasPressed(button)) {
      buttonPressStart[button] = now;
      buttonPressFinish[button] = now;
    }
    if (inputMgr.wasReleased(button)) {
      buttonPressFinish[button] = now;
    }
  }

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  uint8_t pressedMask = 0;
  uint8_t releasedMask = 0;
  uint8_t committedMask = 0;
  for (size_t button = 0; button < BUTTON_COUNT; ++button) {
    if (inputMgr.wasPressed(button)) pressedMask |= static_cast<uint8_t>(1U << button);
    if (inputMgr.wasReleased(button)) releasedMask |= static_cast<uint8_t>(1U << button);
    if (inputMgr.isPressed(button)) committedMask |= static_cast<uint8_t>(1U << button);
  }
  InputManager::ButtonAdcSample group1{};
  InputManager::ButtonAdcSample group2{};
  inputMgr.readButtonAdc(group1, group2);
  static bool adcSampleSeen = false;
  static int previousGroup1Raw = -1;
  static int previousGroup2Raw = -1;
  static int previousGroup1Button = -1;
  static int previousGroup2Button = -1;
  constexpr int ADC_LOG_DELTA = 96;
  const auto rawMoved = [](const int current, const int previous) {
    if (current < 0 || previous < 0) return false;
    const int delta = current >= previous ? current - previous : previous - current;
    return delta >= ADC_LOG_DELTA;
  };
  const bool adcChanged = !adcSampleSeen || group1.button != previousGroup1Button ||
                          group2.button != previousGroup2Button || rawMoved(group1.raw, previousGroup1Raw) ||
                          rawMoved(group2.raw, previousGroup2Raw);
  if ((group1.raw >= 0 || group2.raw >= 0) && adcChanged) {
    LOG_DBG("INP", "stage=adc adc1=%d class1=%d adc2=%d class2=%d", group1.raw, group1.button, group2.raw,
            group2.button);
  }
  adcSampleSeen = true;
  previousGroup1Raw = group1.raw;
  previousGroup2Raw = group2.raw;
  previousGroup1Button = group1.button;
  previousGroup2Button = group2.button;

  const bool debouncePending = inputMgr.isDebouncePending();
  if (debouncePending != debounceWasPending || pressedMask != 0 || releasedMask != 0) {
    const char* const stage = pressedMask != 0 || releasedMask != 0 ? "commit"
                              : debouncePending                     ? "candidate"
                                                                    : "candidate_cleared";
    LOG_DBG("INP", "stage=%s pending=%u state=%02x pressed=%02x released=%02x adc1=%d class1=%d adc2=%d class2=%d",
            stage, debouncePending ? 1U : 0U, committedMask, pressedMask, releasedMask, group1.raw, group1.button,
            group2.raw, group2.button);
  }
#endif

  if (deviceIsX3() && usbPollAttempted && now - lastUsbPollMs < X3_USB_POLL_INTERVAL_MS) {
    return;
  }

  usbPollAttempted = true;
  lastUsbPollMs = now;
  bool connected = false;
  if (!readUsbConnectedNow(connected)) {
    return;
  }

  usbStateChanged = usbSampleValid && connected != usbConnected.load(std::memory_order_relaxed);
  usbConnected.store(connected, std::memory_order_relaxed);
  usbSampleValid = true;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

bool HalGPIO::isDebouncePending() const { return inputMgr.isDebouncePending(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getHeldTime(const uint8_t buttonIndex) const {
  if (buttonIndex >= BUTTON_COUNT) return 0;
  if (inputMgr.isPressed(buttonIndex)) return millis() - buttonPressStart[buttonIndex];
  return buttonPressFinish[buttonIndex] - buttonPressStart[buttonIndex];
}

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return true;
  }

  // A wake may only be authorized by the physical press that is still held
  // when verification begins. Reading the raw state first prevents a later
  // tap from being mistaken for the original wake press while the debouncer is
  // waiting for a new edge.
  constexpr uint8_t POWER_MASK = 1U << BTN_POWER;
  if ((inputMgr.getState() & POWER_MASK) == 0) {
    return false;
  }

  const unsigned long holdStarted = millis();
  while (true) {
    inputMgr.update();
    if ((inputMgr.getState() & POWER_MASK) == 0) {
      return false;
    }
    if (millis() - holdStarted >= requiredDurationMs) {
      return true;
    }
    delay(10);
  }
}

bool HalGPIO::readUsbConnectedNow(bool& connected) const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    int16_t currentMa = 0;
    if (!X3GPIO::readBQ27220CurrentMA(&currentMa)) return false;
    connected = currentMa > 0;
    return true;
  }
  // U0RXD/GPIO20 reads HIGH when USB is connected
  connected = digitalRead(UART0_RXD) == HIGH;
  return true;
}

bool HalGPIO::isUsbConnected() const { return usbConnected.load(std::memory_order_relaxed); }

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // Boot classification must not depend on a potentially stale runtime cache.
  // Seed the cache here so UI reads stay I2C-free after setup.
  bool connected = usbConnected.load(std::memory_order_relaxed);
  usbPollAttempted = true;
  lastUsbPollMs = millis();
  if (readUsbConnectedNow(connected)) {
    usbConnected.store(connected, std::memory_order_relaxed);
    usbSampleValid = true;
  }
  const bool usbConnectedNow = usbSampleValid && usbConnected.load(std::memory_order_relaxed);

  // HalPowerManager arms only the physical power-button GPIO before deep
  // sleep.  Its wake cause is therefore authoritative; the X3 fuel gauge can
  // legitimately report 0 mA during the first boot samples.
  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnectedNow) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnectedNow) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnectedNow) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
