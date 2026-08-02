#pragma once

#include <EpdFontFamily.h>

#include <functional>

#include "components/themes/crossvi/CrossViTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return CrossViMetrics::values; }
  const BaseTheme& getTheme() const { return theme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  CrossViTheme theme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
