#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class GfxRenderer {
 public:
  GfxRenderer(const int width, const int height)
      : width_(width), height_(height), buffer_(static_cast<size_t>(width) * height / 8U, 0xFF) {}

  const uint8_t* getFrameBuffer() const { return buffer_.data(); }
  uint8_t* getFrameBuffer() { return buffer_.data(); }
  size_t getBufferSize() const { return buffer_.size(); }
  int getScreenWidth() const { return width_; }
  int getScreenHeight() const { return height_; }

 private:
  int width_;
  int height_;
  std::vector<uint8_t> buffer_;
};
