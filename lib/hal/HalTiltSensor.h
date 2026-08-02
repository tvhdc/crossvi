#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"
#include "TiltPageTurnPolicy.h"

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_ON = 1, TILT_LEGACY_LEFT_NEXT = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization
  TiltPageTurnPolicy::GestureState _gesture;

  // Tuning constants
  static constexpr unsigned long POLL_INTERVAL_MS = 100;   // 10 Hz polling; gestures last 300-700 ms
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;  // Ignore readings after wake
  static constexpr unsigned long SLEEP_STABILIZE_MS = 15;  // Sleep turn on/off delay

  mutable unsigned long _lastPollMs = 0;

  // --- QMI8658 registers ---
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL2 = 0x03;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_AX_L = 0x35;

  // --- Register Bit Flags ---

  // REG_CTRL1 (0x02)
  static constexpr uint8_t CTRL1_BIG_ENDIAN = (1 << 5);                     // 0x20: Default state (1 = Big Endian)
  static constexpr uint8_t CTRL1_AUTO_INC = (1 << 6);                       // 0x40: Enable address auto-increment
  static constexpr uint8_t CTRL1_SENSOR_DISABLE = (1 << 0);                 // 0x01: Power down sensor engine
  static constexpr uint8_t CTRL1_BASE = CTRL1_AUTO_INC | CTRL1_BIG_ENDIAN;  // 0x60

  // REG_CTRL2 (0x03) - Accelerometer Config
  static constexpr uint8_t CTRL2_FS_2G = 0;
  static constexpr uint8_t CTRL2_ODR_11HZ_LOW_POWER = 0b1110;

  // REG_CTRL7 (0x08) - Enable
  static constexpr uint8_t CTRL7_DISABLE_ALL = 0x00;
  static constexpr uint8_t CTRL7_ACCEL_ENABLE = (1 << 0);

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readAccelerometer(float& ax, float& ay, float& az) const;

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // Enables the QMI8658 internal sensor engine
  bool wake();

  // Puts the QMI8658 into a low-power standby state
  bool deepSleep();

  // True if the QMI8658 IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update tilt gesture state.
  void update(uint8_t mode, uint8_t orientation, bool inReader, bool pageReady, uint32_t completedRenderGeneration);

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
