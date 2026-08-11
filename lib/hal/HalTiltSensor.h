#pragma once

#include <Arduino.h>
#include <Imu.h>

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  mutable Imu _sdkImu;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization

  // CrossPoint's original gyro gesture tuning.
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;
  static constexpr unsigned long COOLDOWN_MS = 600;
  static constexpr unsigned long POLL_INTERVAL_MS = 50;
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;

  mutable unsigned long _lastPollMs = 0;

  bool readGyro(float& gx, float& gy, float& gz) const;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Enables tilt polling state.
  bool wake();

  // Puts tilt polling state to sleep.
  bool deepSleep();

  // True if an IMU is present on this device.
  bool isAvailable() const { return _available; }

  // Poll the gyro and update tilt gesture state.
  void update(uint8_t mode, uint8_t orientation, bool inReader);

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events.
  void clearPendingEvents();
};
