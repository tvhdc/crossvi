#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class GfxRenderer {
 public:
  GfxRenderer(const int portraitWidth, const int portraitHeight, const bool landscape = false)
      : portraitWidth_(portraitWidth),
        portraitHeight_(portraitHeight),
        landscape_(landscape),
        buffer_(static_cast<size_t>(portraitWidth) * portraitHeight / 8U, 0xFF) {}

  const uint8_t* getFrameBuffer() const { return buffer_.data(); }
  uint8_t* getFrameBuffer() { return buffer_.data(); }
  size_t getBufferSize() const { return buffer_.size(); }
  int getScreenWidth() const { return landscape_ ? portraitHeight_ : portraitWidth_; }
  int getScreenHeight() const { return landscape_ ? portraitWidth_ : portraitHeight_; }
  uint16_t getDisplayWidth() const { return static_cast<uint16_t>(portraitHeight_); }
  uint16_t getDisplayHeight() const { return static_cast<uint16_t>(portraitWidth_); }

 private:
  int portraitWidth_;
  int portraitHeight_;
  bool landscape_;
  std::vector<uint8_t> buffer_;
};
