#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class HalDisplay {
 public:
  // Production HalDisplay reports the panel's physical landscape geometry,
  // while SleepFrameStore persists portrait-oriented frame metadata.
  HalDisplay(const uint16_t portraitWidth, const uint16_t portraitHeight)
      : width_(portraitHeight),
        height_(portraitWidth),
        buffer_(static_cast<size_t>(portraitWidth) * portraitHeight / 8U, 0) {}

  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(buffer_.size()); }
  uint8_t* getFrameBuffer() { return buffer_.data(); }

 private:
  uint16_t width_;
  uint16_t height_;
  std::vector<uint8_t> buffer_;
};
