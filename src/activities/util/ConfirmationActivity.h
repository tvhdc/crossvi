#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "fontIds.h"
#include "util/PressReleaseLatch.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;
  std::string negativeLabel;
  std::string positiveLabel;
  StrId positiveFeedback;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  int startY = 0;
  int lineHeight = 0;
  InputIdleGate inputGate;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, std::string negativeLabel = {}, std::string positiveLabel = {},
                       StrId positiveFeedback = StrId::_COUNT);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
