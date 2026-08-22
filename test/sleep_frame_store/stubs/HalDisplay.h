#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class HalDisplay {
 public:
  HalDisplay(const uint16_t width, const uint16_t height)
      : width_(width), height_(height), buffer_(static_cast<size_t>(width) * height / 8U, 0) {}

  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(buffer_.size()); }
  uint8_t* getFrameBuffer() { return buffer_.data(); }

 private:
  uint16_t width_;
  uint16_t height_;
  std::vector<uint8_t> buffer_;
};
