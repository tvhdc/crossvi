#include "SleepImagePositionActivity.h"

#include <Epub/converters/PngToFramebufferConverter.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "Bitmap.h"
#include "FsHelpers.h"
#include "activities/ActivityResult.h"
#include "activities/boot_sleep/SleepFrameStore.h"
#include "activities/boot_sleep/SleepImagePlacement.h"
#include "activities/boot_sleep/SleepImageSelectionStore.h"
#include "activities/boot_sleep/SleepImageValidation.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void drawDashedRect(const GfxRenderer& renderer, const int x, const int y, const int width, const int height) {
  if (width <= 0 || height <= 0) return;

  constexpr int DASH = 7;
  constexpr int PERIOD = 12;
  const int right = x + width - 1;
  const int bottom = y + height - 1;
  for (int px = x; px <= right; px += PERIOD) {
    const int end = std::min(px + DASH - 1, right);
    renderer.drawLine(px, y, end, y);
    renderer.drawLine(px, bottom, end, bottom);
  }
  for (int py = y; py <= bottom; py += PERIOD) {
    const int end = std::min(py + DASH - 1, bottom);
    renderer.drawLine(x, py, x, end);
    renderer.drawLine(right, py, right, end);
  }
}

bool bmpDimensions(const std::string& path, int& width, int& height) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;
  SleepImageValidation::Bmp32Header alpha;
  const auto alphaStatus = SleepImageValidation::readBmp32Header(file, alpha);
  if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Valid) {
    width = alpha.width;
    height = alpha.height;
    return file.close();
  }
  if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Invalid || !file.seek(0)) {
    file.close();
    return false;
  }
  Bitmap bitmap(file, true);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (valid) {
    width = bitmap.getWidth();
    height = bitmap.getHeight();
  }
  return file.close() && valid;
}

bool pngDimensions(const std::string& path, int& width, int& height) {
  ImageDimensions dimensions{};
  if (!PngToFramebufferConverter::getSupportedDimensionsStatic(path, dimensions)) return false;
  width = dimensions.width;
  height = dimensions.height;
  return width > 0 && height > 0;
}
}  // namespace

void SleepImagePositionActivity::onEnter() {
  Activity::onEnter();
  catalog_ = {};
  imageAvailable_ = false;
  saveFailed_ = SleepImageSelectionStore::loadCatalog(catalog_) != SleepImageSelectionStore::CatalogStatus::Ok;
  if (const auto* image = SleepImageSelectionStore::findImage(catalog_, imageId_)) {
    sourcePath_ = image->path;
    zoom_ = std::clamp<uint8_t>(image->transform.zoom, SLEEP_IMAGE_MIN_ZOOM, SLEEP_IMAGE_MAX_ZOOM);
    offsetX_ = static_cast<int16_t>(
        std::clamp<int>(image->transform.offsetX, -renderer.getScreenWidth() / 2, renderer.getScreenWidth() / 2));
    offsetY_ = static_cast<int16_t>(
        std::clamp<int>(image->transform.offsetY, -renderer.getScreenHeight() / 2, renderer.getScreenHeight() / 2));
    initialZoom_ = zoom_;
    initialOffsetX_ = offsetX_;
    initialOffsetY_ = offsetY_;
    imageAvailable_ = true;
  } else {
    sourcePath_.clear();
  }
  resetHoldHandled_ = false;
  resolveSourceGeometry();
  requestUpdate();
}

void SleepImagePositionActivity::resolveSourceGeometry() {
  sourceWidth_ = 0;
  sourceHeight_ = 0;
  if (sourcePath_.empty() || !Storage.exists(sourcePath_.c_str())) return;
  bool loaded = false;
  if (FsHelpers::hasPngExtension(sourcePath_)) {
    loaded = pngDimensions(sourcePath_, sourceWidth_, sourceHeight_);
  } else if (FsHelpers::hasBmpExtension(sourcePath_)) {
    loaded = bmpDimensions(sourcePath_, sourceWidth_, sourceHeight_);
  }
  if (!loaded) {
    sourceWidth_ = 0;
    sourceHeight_ = 0;
  }
}

void SleepImagePositionActivity::adjustZoom(const int delta) {
  const uint8_t nextZoom = static_cast<uint8_t>(std::clamp(
      static_cast<int>(zoom_) + delta, static_cast<int>(SLEEP_IMAGE_MIN_ZOOM), static_cast<int>(SLEEP_IMAGE_MAX_ZOOM)));
  if (nextZoom == zoom_) return;
  zoom_ = nextZoom;
  requestUpdate();
}

void SleepImagePositionActivity::move(const int dx, const int dy) {
  const int16_t nextX = static_cast<int16_t>(
      std::clamp(static_cast<int>(offsetX_) + dx, -renderer.getScreenWidth() / 2, renderer.getScreenWidth() / 2));
  const int16_t nextY = static_cast<int16_t>(
      std::clamp(static_cast<int>(offsetY_) + dy, -renderer.getScreenHeight() / 2, renderer.getScreenHeight() / 2));
  if (nextX == offsetX_ && nextY == offsetY_) return;
  offsetX_ = nextX;
  offsetY_ = nextY;
  requestUpdate();
}

int SleepImagePositionActivity::moveStep(const MappedInputManager::Button button) const {
  return sleepImageMoveStep(mappedInput.getHeldTime(button));
}

void SleepImagePositionActivity::resetTransform() {
  zoom_ = SLEEP_IMAGE_DEFAULT_ZOOM;
  offsetX_ = 0;
  offsetY_ = 0;
  resetHoldHandled_ = true;
  requestUpdate();
}

void SleepImagePositionActivity::saveAndFinish() {
  if (!imageAvailable_) {
    saveFailed_ = true;
    requestUpdate();
    return;
  }
  if (zoom_ == initialZoom_ && offsetX_ == initialOffsetX_ && offsetY_ == initialOffsetY_) {
    setResult(ActivityResult{});
    finish();
    return;
  }
  const SleepImageSelectionStore::ImageTransform transform{zoom_, offsetX_, offsetY_};
  if (SleepImageSelectionStore::updateTransform(catalog_, imageId_, transform) !=
      SleepImageSelectionStore::CatalogStatus::Ok) {
    saveFailed_ = true;
    requestUpdate();
    return;
  }
  SleepFrameStore::discard();
  setResult(ActivityResult{});
  finish();
}

void SleepImagePositionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (!resetHoldHandled_ && (mappedInput.isPressed(MappedInputManager::Button::Confirm) || confirmReleased) &&
      shouldResetSleepImageTransform(mappedInput.getHeldTime(MappedInputManager::Button::Confirm))) {
    resetTransform();
  }
  if (confirmReleased) {
    resetHoldHandled_ = false;
    saveAndFinish();
    return;
  }

  if (mode_ == Mode::Zoom) {
    buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Left},
                                          [this] { adjustZoom(-SLEEP_IMAGE_FRONT_ZOOM_STEP); });
    buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Right},
                                          [this] { adjustZoom(SLEEP_IMAGE_FRONT_ZOOM_STEP); });
    const int upDelta = gpio.deviceIsX3() ? -SLEEP_IMAGE_SIDE_ZOOM_STEP : SLEEP_IMAGE_SIDE_ZOOM_STEP;
    const int downDelta = gpio.deviceIsX3() ? SLEEP_IMAGE_SIDE_ZOOM_STEP : -SLEEP_IMAGE_SIDE_ZOOM_STEP;
    buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustZoom(upDelta); });
    buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Down},
                                          [this, downDelta] { adjustZoom(downDelta); });
    return;
  }

  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Left},
                                        [this] { move(-moveStep(MappedInputManager::Button::Left), 0); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Right},
                                        [this] { move(moveStep(MappedInputManager::Button::Right), 0); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Up},
                                        [this] { move(0, -moveStep(MappedInputManager::Button::Up)); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Down},
                                        [this] { move(0, moveStep(MappedInputManager::Button::Down)); });
}

void SleepImagePositionActivity::drawZoomStepHint(const int y, const StrId labelId, const int step) {
  char line[64];
  snprintf(line, sizeof(line), "%s %d%%", I18N.get(labelId), step);
  renderer.drawCenteredText(SMALL_FONT_ID, y, line, true);
}

void SleepImagePositionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const StrId titleId = mode_ == Mode::Zoom ? StrId::STR_SLEEP_IMAGE_ZOOM : StrId::STR_SLEEP_IMAGE_POSITION;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(titleId));

  const int previewTop = metrics.topPadding + metrics.headerHeight + 78;
  const int previewWidth = std::max(120, pageWidth - 60);
  const int previewHeight = std::min(300, std::max(140, pageHeight - previewTop - 135));
  const int previewX = (pageWidth - previewWidth) / 2;
  const bool hasSourceGeometry = sourceWidth_ > 0 && sourceHeight_ > 0;
  const SleepImagePlacement image =
      hasSourceGeometry
          ? calculateSleepImagePlacement(pageWidth, pageHeight, sourceWidth_, sourceHeight_, zoom_, offsetX_, offsetY_)
          : SleepImagePlacement{};

  const int boundsLeft = hasSourceGeometry ? std::min(0, image.x) : 0;
  const int boundsTop = hasSourceGeometry ? std::min(0, image.y) : 0;
  const int boundsRight = hasSourceGeometry ? std::max(pageWidth, image.x + image.width) : pageWidth;
  const int boundsBottom = hasSourceGeometry ? std::max(pageHeight, image.y + image.height) : pageHeight;
  const float previewScale = std::min(static_cast<float>(previewWidth) / (boundsRight - boundsLeft),
                                      static_cast<float>(previewHeight) / (boundsBottom - boundsTop));
  const int usedWidth = std::max(1, static_cast<int>(std::lround((boundsRight - boundsLeft) * previewScale)));
  const int usedHeight = std::max(1, static_cast<int>(std::lround((boundsBottom - boundsTop) * previewScale)));
  const int originX = previewX + (previewWidth - usedWidth) / 2;
  const int originY = previewTop + (previewHeight - usedHeight) / 2;
  const auto mapX = [&](const int value) {
    return originX + static_cast<int>(std::lround((value - boundsLeft) * previewScale));
  };
  const auto mapY = [&](const int value) {
    return originY + static_cast<int>(std::lround((value - boundsTop) * previewScale));
  };

  renderer.drawRect(mapX(0), mapY(0), std::max(1, mapX(pageWidth) - mapX(0)), std::max(1, mapY(pageHeight) - mapY(0)),
                    2, true);
  if (hasSourceGeometry) {
    drawDashedRect(renderer, mapX(image.x), mapY(image.y), std::max(1, mapX(image.x + image.width) - mapX(image.x)),
                   std::max(1, mapY(image.y + image.height) - mapY(image.y)));
  }

  renderer.drawCenteredText(SMALL_FONT_ID, previewTop - 48, tr(STR_SLEEP_IMAGE_SCREEN_FRAME_HINT));
  renderer.drawCenteredText(SMALL_FONT_ID, previewTop - 26, tr(STR_SLEEP_IMAGE_FRAME_HINT));
  if (!hasSourceGeometry) {
    renderer.drawCenteredText(SMALL_FONT_ID, previewTop + previewHeight / 2, tr(STR_SLEEP_IMAGE_SIZE_UNAVAILABLE));
  }

  const int footerTop = previewTop + previewHeight + 24;
  if (mode_ == Mode::Zoom) {
    char value[16];
    snprintf(value, sizeof(value), tr(STR_PERCENT_VALUE_FORMAT), static_cast<unsigned>(zoom_));
    renderer.drawCenteredText(UI_10_FONT_ID, footerTop, value, true, EpdFontFamily::BOLD);
    drawZoomStepHint(footerTop + 25, StrId::STR_STEP_HINT_FRONT, SLEEP_IMAGE_FRONT_ZOOM_STEP);
    drawZoomStepHint(footerTop + 47, StrId::STR_STEP_HINT_SIDE, SLEEP_IMAGE_SIDE_ZOOM_STEP);
    renderer.drawCenteredText(SMALL_FONT_ID, footerTop + 69, tr(STR_SLEEP_IMAGE_RESET_HINT));
  } else {
    renderer.drawCenteredText(SMALL_FONT_ID, footerTop, tr(STR_SLEEP_IMAGE_MOVE_HINT));
    renderer.drawCenteredText(SMALL_FONT_ID, footerTop + 22, tr(STR_SLEEP_IMAGE_RESET_HINT));
  }

  const auto labels = mode_ == Mode::Zoom
                          ? mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DONE), "-", "+")
                          : mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DONE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (saveFailed_) {
    saveFailed_ = false;
    drawTransientPopup(StrId::STR_SLEEP_IMAGE_SAVE_FAILED);
    return;
  }
  renderer.displayBuffer();
}
