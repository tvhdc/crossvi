#include "BootActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Version.h>

#include "fontIds.h"
#include "images/DefaultSleepScreens.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  if (minimalWakeScreen_) {
    const int bootingY = (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, bootingY, tr(STR_BOOTING), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  const bool isX3 = gpio.deviceIsX3();
  const bool hasBundledScreen =
      (isX3 && pageWidth == DEFAULT_SLEEP_X3_WIDTH && pageHeight == DEFAULT_SLEEP_X3_HEIGHT) ||
      (!isX3 && pageWidth == DEFAULT_SLEEP_X4_WIDTH && pageHeight == DEFAULT_SLEEP_X4_HEIGHT);
  if (hasBundledScreen) {
    if (isX3) {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X3_WIDTH, DEFAULT_SLEEP_X3_HEIGHT, DefaultSleepX3Rows,
                               DefaultSleepX3Runs);
    } else {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X4_WIDTH, DEFAULT_SLEEP_X4_HEIGHT, DefaultSleepX4Rows,
                               DefaultSleepX4Runs);
    }
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 70, tr(STR_BOOTING));
  } else {
    // Keep a text-only fallback for an unexpected orientation or panel.
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2, tr(STR_BOOTING),
                              true, EpdFontFamily::BOLD);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
