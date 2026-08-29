#include "HalTiltSensor.h"

#include <Logging.h>

#include <cmath>

#include "TiltLifecyclePolicy.h"

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  gx = sample.gx;
  gy = sample.gy;
  gz = sample.gz;
  return true;
}

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (_available) {
    _lastPollMs = millis();
    // Keep the IMU in standby until tilt page turning is enabled.
    if (!_sdkImu.sleep()) {
      LOG_ERR("GYR", "IMU standby failed");
    }
    LOG_INF("GYR", "SDK IMU initialized");
    return;
  }
  LOG_ERR("GYR", "SDK IMU not found");
}

bool HalTiltSensor::wake() {
  if (!_available) return false;

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) return false;

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!_available) return;

  const bool shouldBeAwake = TiltLifecyclePolicy::shouldBeAwake(mode, inReader);
  if (shouldBeAwake && !_isAwake) {
    _isAwake = wake();
    return;
  } else if (!shouldBeAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  if (!shouldBeAwake) return;

  const unsigned long now = millis();
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) return;
  if ((now - _lastPollMs) < POLL_INTERVAL_MS) return;
  _lastPollMs = now;

  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) return;

  // Map the gyro axis to left/right tilt based on reader orientation.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gx : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gx : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gy : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gy : gy;
      break;
    default:
      tiltAxis = gx;
      break;
  }

  if (_inTilt) {
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) _inTilt = false;
  } else if ((now - _lastTiltMs) >= COOLDOWN_MS) {
    if (tiltAxis > RATE_THRESHOLD_DPS) {
      _tiltForwardEvent = true;
      _hadActivity = true;
      _inTilt = true;
      _lastTiltMs = now;
      LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
    } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
      _tiltBackEvent = true;
      _hadActivity = true;
      _inTilt = true;
      _lastTiltMs = now;
      LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Preserve _inTilt so a held tilt cannot retrigger on the next poll.
}
