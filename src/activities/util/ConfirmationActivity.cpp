#include "ConfirmationActivity.h"

#include <I18n.h>

#include <utility>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           std::string negativeLabel, std::string positiveLabel,
                                           const StrId positiveFeedback)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      negativeLabel(std::move(negativeLabel)),
      positiveLabel(std::move(positiveLabel)),
      positiveFeedback(positiveFeedback) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  inputGate.reset();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate();
}

void ConfirmationActivity::render(RenderLock&& lock) {
  if (renderBlockingFeedbackOverlay()) return;
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Draw UI Elements
  const char* cancel = negativeLabel.empty() ? I18N.get(StrId::STR_CANCEL) : negativeLabel.c_str();
  const char* confirm = positiveLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : positiveLabel.c_str();
  const auto labels = mappedInput.mapLabels("", "", cancel, confirm);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  const bool anyFrontPressed = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                               mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                               mappedInput.isPressed(MappedInputManager::Button::Left) ||
                               mappedInput.isPressed(MappedInputManager::Button::Right);
  // A dialog can be pushed while the button that opened it is still held.
  // Wait for one idle sample so its release cannot choose an answer here.
  if (!inputGate.update(anyFrontPressed, mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased())) return;

  if (positiveFeedback != StrId::_COUNT && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    queueBlockingFeedback(positiveFeedback);
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
