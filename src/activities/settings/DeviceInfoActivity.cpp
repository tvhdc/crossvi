#include "DeviceInfoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Version.h>

#include <array>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

#ifndef SIMULATOR
#include <BoardConfig.h>
#endif

namespace {
#ifndef SIMULATOR
const char* controllerName(const BoardConfig::DisplayController controller) {
  switch (controller) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::LgfxEpd:
    default:
      return "LgfxEpd";
  }
}
#endif

std::string formatMegabytes(const uint64_t bytes) {
  if (bytes == 0) return "--";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buffer;
}
}  // namespace

void DeviceInfoActivity::onEnter() {
  Activity::onEnter();
  // freeClusterCount(), used by Storage.usedBytes(), walks the FAT and can
  // block this informational screen for seconds on a large card.  The card
  // was already probed before Settings opened, so only show the cached total.
  storageAvailable = Storage.ready();
  if (storageAvailable) {
    sdTotalBytes = Storage.totalBytes();
  }
  requestUpdate();
}

void DeviceInfoActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
}

void DeviceInfoActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_DEVICE_INFO));

  std::string board;
  std::string displaySize;
#ifdef SIMULATOR
  board = "Simulator";
  displaySize = std::to_string(renderer.getScreenWidth()) + "×" + std::to_string(renderer.getScreenHeight());
#else
  board = std::string(BoardConfig::ACTIVE.name) + " / " + controllerName(BoardConfig::ACTIVE.displayController);
  displaySize =
      std::to_string(BoardConfig::ACTIVE.displayWidth) + "×" + std::to_string(BoardConfig::ACTIVE.displayHeight);
#endif
  std::array<std::string, 4> rows = {
      std::string(tr(STR_DEVICE_BOARD)) + ": " + board,
      std::string(tr(STR_DEVICE_DISPLAY)) + ": " + displaySize,
      std::string(tr(STR_DEVICE_FIRMWARE)) + ": " + CROSSPOINT_VERSION,
      std::string(tr(STR_DEVICE_STORAGE)) + ": " + (storageAvailable ? formatMegabytes(sdTotalBytes) : "--"),
  };
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{0, top, width, std::max(1, bottom - top)}, rows.size(), -1,
               [&rows](int index) { return rows[static_cast<size_t>(index)]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
