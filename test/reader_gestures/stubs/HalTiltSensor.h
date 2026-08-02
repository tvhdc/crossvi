#pragma once

class HalTiltSensor {
 public:
  bool forward = false;
  bool back = false;
  bool wasTiltedForward() {
    const bool result = forward;
    forward = false;
    return result;
  }
  bool wasTiltedBack() {
    const bool result = back;
    back = false;
    return result;
  }
};

extern HalTiltSensor halTiltSensor;
