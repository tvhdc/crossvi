#pragma once

#include <cstddef>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FinishedBooksActivity final : public Activity {
 public:
  FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FinishedBooks", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void move(int delta);

  ButtonNavigator navigator_;
  size_t selected_ = 0;
};
