#pragma once

#include <EpdFontFamily.h>

#include <cstring>

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  GfxRenderer(const int width = 480, const int height = 800, const int lineHeight = 24)
      : width_(width), height_(height), lineHeight_(lineHeight) {}

  int getScreenWidth() const { return width_; }
  int getScreenHeight() const { return height_; }
  int getLineHeight(int) const { return lineHeight_; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    return static_cast<int>(std::strlen(text)) * 6;
  }
  void drawLine(int, int, int, int, int, bool) const { ++underlineCount_; }
  void fillRectDither(int, int, int, int, Color) const { ++backgroundCount_; }
  int underlineCount() const { return underlineCount_; }
  int backgroundCount() const { return backgroundCount_; }

 private:
  int width_;
  int height_;
  int lineHeight_;
  mutable int underlineCount_ = 0;
  mutable int backgroundCount_ = 0;
};
