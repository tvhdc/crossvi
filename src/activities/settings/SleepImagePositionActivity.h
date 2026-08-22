#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SleepImagePositionActivity final : public Activity {
 public:
  enum class Mode { Zoom, Position };

  SleepImagePositionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode)
      : Activity(mode == Mode::Zoom ? "SleepImageZoom" : "SleepImagePosition", renderer, mappedInput), mode_(mode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Mode mode_;
  uint8_t zoom_ = 100;
  int16_t offsetX_ = 0;
  int16_t offsetY_ = 0;
  int sourceWidth_ = 0;
  int sourceHeight_ = 0;
  bool sourceSizeVaries_ = false;
  bool resetHoldHandled_ = false;
  ButtonNavigator buttonNavigator_;

  void adjustZoom(int delta);
  void move(int dx, int dy);
  int moveStep(MappedInputManager::Button button) const;
  void resetTransform();
  void resolveSourceGeometry();
  void saveAndFinish();
  void drawZoomStepHint(int y, StrId labelId, int step);
};
