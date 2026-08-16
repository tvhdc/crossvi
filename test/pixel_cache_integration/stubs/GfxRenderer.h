#pragma once

class FontCacheManager;

class GfxRenderer {
 public:
  FontCacheManager* getFontCacheManager() { return nullptr; }
  int getScreenWidth() const { return 1000; }
  int getScreenHeight() const { return 1000; }
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }
  void fillRect(int, int, int, int, bool) { ++fillRectCalls; }

  int fillRectCalls = 0;
};
