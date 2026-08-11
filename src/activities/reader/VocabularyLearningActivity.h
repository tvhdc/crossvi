#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class VocabularyLearningActivity final : public Activity {
 public:
  explicit VocabularyLearningActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("VocabularyLearning", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { Settings, Question, Feedback, Results, Review, Source };
  enum class AnswerState : uint8_t { Correct, Wrong, Skipped, TimedOut };

  struct QuestionRecord {
    uint16_t entryIndex = 0;
    uint16_t answerIndices[4]{};
    int8_t selectedSlot = -1;
    uint8_t correctSlot = 0;
    AnswerState state = AnswerState::Skipped;
  };

  static constexpr uint8_t MAX_QUESTIONS = 30;

  Screen screen_ = Screen::Settings;
  OptionPopup optionPopup_;
  ButtonNavigator navigator_;
  int selectedSetting_ = 0;
  int selectedAnswer_ = 0;
  int selectedResultAction_ = 0;
  uint8_t questionCount_ = 0;
  uint8_t answerCount_ = 3;
  uint8_t currentQuestion_ = 0;
  uint8_t reviewIndex_ = 0;
  uint8_t correctCount_ = 0;
  uint8_t wrongCount_ = 0;
  uint8_t skippedCount_ = 0;
  uint32_t randomState_ = 0;
  uint32_t questionStartedAt_ = 0;
  uint8_t countdownSegments_ = 5;
  std::array<uint8_t, 4> answerSlotOrder_{0, 1, 2, 3};
  uint8_t nextAnswerSlot_ = 4;
  ReaderUtils::HoldGestureState skipHold_;
  std::array<QuestionRecord, MAX_QUESTIONS> records_{};

  void handleSettingsInput();
  void handleQuestionInput();
  void handleFeedbackInput();
  void handleResultsInput();
  void handleReviewInput();
  void beginQuiz(uint8_t count);
  void prepareQuestion();
  void submitAnswer(int selectedSlot, bool timedOut = false);
  void advanceAfterFeedback();
  void finishActivity();

  uint8_t configuredQuestionCount() const;
  uint8_t configuredQuestionSeconds() const;
  uint8_t configuredAnswerCount() const;
  const char* quizSizeLabel() const;
  const char* questionTimeLabel() const;
  const char* answerCountLabel() const;

  void renderSettings();
  void renderQuestion(bool showFeedback);
  void renderResults();
  void renderReview();
  void renderSource();
  void drawAnswerCard(int slot, int x, int y, int width, int height, bool showFeedback) const;
  void drawAnswerStateIcon(AnswerState state, int x, int y, int size) const;
  const char* answerStateLabel(AnswerState state) const;
};
