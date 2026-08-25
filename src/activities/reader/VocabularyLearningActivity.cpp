#include "VocabularyLearningActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "vocabulary/VocabularyData.h"
#include "vocabulary/VocabularyQuizTiming.h"
#include "vocabulary/VocabularyReviewStore.h"

namespace {

constexpr std::array<StrId, CrossPointSettings::VOCABULARY_QUIZ_SIZE_COUNT> QUIZ_SIZE_LABELS = {
    StrId::STR_VOCAB_QUESTIONS_5, StrId::STR_VOCAB_QUESTIONS_10, StrId::STR_VOCAB_QUESTIONS_20,
    StrId::STR_VOCAB_QUESTIONS_30};
constexpr std::array<StrId, CrossPointSettings::VOCABULARY_QUESTION_TIME_COUNT> QUESTION_TIME_LABELS = {
    StrId::STR_VOCAB_SECONDS_10, StrId::STR_VOCAB_SECONDS_15, StrId::STR_VOCAB_SECONDS_20, StrId::STR_VOCAB_SECONDS_30,
    StrId::STR_VOCAB_UNLIMITED};
constexpr std::array<StrId, CrossPointSettings::VOCABULARY_ANSWER_COUNT_COUNT> ANSWER_COUNT_LABELS = {
    StrId::STR_VOCAB_ANSWERS_3, StrId::STR_VOCAB_ANSWERS_4};
constexpr std::array<uint8_t, CrossPointSettings::VOCABULARY_QUIZ_SIZE_COUNT> QUIZ_SIZES = {5, 10, 20, 30};
constexpr std::array<uint8_t, CrossPointSettings::VOCABULARY_QUESTION_TIME_COUNT> QUESTION_SECONDS = {10, 15, 20, 30,
                                                                                                      0};
constexpr std::array<uint8_t, CrossPointSettings::VOCABULARY_ANSWER_COUNT_COUNT> ANSWER_COUNTS = {3, 4};

uint32_t nextRandom(uint32_t& state) {
  if (state == 0) state = 0x9E3779B9U;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

void copyDatasetPath(char destination[CrossPointSettings::VOCABULARY_DATASET_PATH_CAPACITY], const char* source) {
  const size_t length = std::min(std::strlen(source), CrossPointSettings::VOCABULARY_DATASET_PATH_CAPACITY - 1);
  std::memcpy(destination, source, length);
  destination[length] = '\0';
}

}  // namespace

void VocabularyLearningActivity::onEnter() {
  Activity::onEnter();
  randomState_ = static_cast<uint32_t>(millis()) ^ 0xC05F17A1U;
  datasetLoadFailed_ = SETTINGS.vocabularyDatasetPath[0] != '\0' &&
                       !crossvi::vocabulary::useExternalDataset(SETTINGS.vocabularyDatasetPath);
  if (datasetLoadFailed_) crossvi::vocabulary::useBuiltInDataset();
  const auto dataset = crossvi::vocabulary::activeDatasetInfo();
  reviewStoreReady_ = VOCABULARY_REVIEW.configure(dataset.identity, dataset.entryCount, dataset.external);
  screen_ = Screen::Settings;
  selectedSetting_ = 0;
  skipHold_.reset();
  requestUpdate();
}

void VocabularyLearningActivity::onExit() {
  if (reviewStoreReady_ && !VOCABULARY_REVIEW.flush()) LOG_ERR("VOCAB", "Failed to save review words on exit");
  crossvi::vocabulary::useBuiltInDataset();
  Activity::onExit();
}

uint8_t VocabularyLearningActivity::configuredQuestionCount() const {
  const size_t index = std::min<size_t>(SETTINGS.vocabularyQuizSize, QUIZ_SIZES.size() - 1);
  return QUIZ_SIZES[index];
}

uint8_t VocabularyLearningActivity::configuredQuestionSeconds() const {
  const size_t index = std::min<size_t>(SETTINGS.vocabularyQuestionTime, QUESTION_SECONDS.size() - 1);
  return QUESTION_SECONDS[index];
}

uint8_t VocabularyLearningActivity::configuredAnswerCount() const {
  const size_t index = std::min<size_t>(SETTINGS.vocabularyAnswerCount, ANSWER_COUNTS.size() - 1);
  return ANSWER_COUNTS[index];
}

const char* VocabularyLearningActivity::quizSizeLabel() const {
  return I18N.get(QUIZ_SIZE_LABELS[std::min<size_t>(SETTINGS.vocabularyQuizSize, QUIZ_SIZE_LABELS.size() - 1)]);
}

const char* VocabularyLearningActivity::questionTimeLabel() const {
  return I18N.get(
      QUESTION_TIME_LABELS[std::min<size_t>(SETTINGS.vocabularyQuestionTime, QUESTION_TIME_LABELS.size() - 1)]);
}

const char* VocabularyLearningActivity::answerCountLabel() const {
  return I18N.get(
      ANSWER_COUNT_LABELS[std::min<size_t>(SETTINGS.vocabularyAnswerCount, ANSWER_COUNT_LABELS.size() - 1)]);
}

void VocabularyLearningActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  switch (screen_) {
    case Screen::Settings:
      handleSettingsInput();
      break;
    case Screen::Question:
      handleQuestionInput();
      break;
    case Screen::Feedback:
      handleFeedbackInput();
      break;
    case Screen::Results:
      handleResultsInput();
      break;
    case Screen::Review:
      handleReviewInput();
      break;
    case Screen::Source:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        screen_ = Screen::Settings;
        requestUpdate();
      }
      break;
  }
}

void VocabularyLearningActivity::handleSettingsInput() {
  constexpr int ITEM_COUNT = 7;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishActivity();
    return;
  }
  navigator_.onNext([this] {
    selectedSetting_ = ButtonNavigator::nextIndex(selectedSetting_, ITEM_COUNT);
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    selectedSetting_ = ButtonNavigator::previousIndex(selectedSetting_, ITEM_COUNT);
    requestUpdate();
  });
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;

  switch (selectedSetting_) {
    case 0:
      beginQuiz(configuredQuestionCount());
      break;
    case 1:
      beginReviewQuiz();
      break;
    case 2:
      showDatasetPicker();
      break;
    case 3:
      optionPopup_.show(StrId::STR_VOCAB_QUIZ_SIZE, QUIZ_SIZE_LABELS.data(), QUIZ_SIZE_LABELS.size(),
                        SETTINGS.vocabularyQuizSize, [this](const int index) {
                          SETTINGS.vocabularyQuizSize = static_cast<uint8_t>(index);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case 4:
      optionPopup_.show(StrId::STR_VOCAB_QUESTION_TIME, QUESTION_TIME_LABELS.data(), QUESTION_TIME_LABELS.size(),
                        SETTINGS.vocabularyQuestionTime, [this](const int index) {
                          SETTINGS.vocabularyQuestionTime = static_cast<uint8_t>(index);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case 5:
      optionPopup_.show(StrId::STR_VOCAB_ANSWER_COUNT, ANSWER_COUNT_LABELS.data(), ANSWER_COUNT_LABELS.size(),
                        SETTINGS.vocabularyAnswerCount, [this](const int index) {
                          SETTINGS.vocabularyAnswerCount = static_cast<uint8_t>(index);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case 6:
      screen_ = Screen::Source;
      requestUpdate();
      break;
  }
}

void VocabularyLearningActivity::showDatasetPicker() {
  const int selected = crossvi::vocabulary::activeDatasetInfo().external ? 1 : 0;
  optionPopup_.show(StrId::STR_VOCAB_SET,
                    std::vector<std::string>{tr(STR_VOCAB_BUILT_IN_SET), tr(STR_VOCAB_CHOOSE_FILE)}, selected,
                    [this](const int index) {
                      if (index == 0) {
                        selectDataset(nullptr);
                        requestUpdate();
                        return;
                      }
                      openDatasetFilePicker();
                    });
  requestUpdate();
}

void VocabularyLearningActivity::openDatasetFilePicker() {
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickVocabulary),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* selected = std::get_if<FilePathResult>(&result.data);
        if (!selected) return;
        GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        if (!selectDataset(selected->path.c_str())) GUI.drawPopup(renderer, tr(STR_VOCAB_SET_INVALID));
      });
}

bool VocabularyLearningActivity::selectDataset(const char* path) {
  if (reviewStoreReady_ && !VOCABULARY_REVIEW.flush()) return false;
  const std::string previousPath = SETTINGS.vocabularyDatasetPath;
  const bool external = path && path[0] != '\0';
  const bool activated =
      external ? crossvi::vocabulary::useExternalDataset(path) : (crossvi::vocabulary::useBuiltInDataset(), true);
  if (!activated) return false;

  const auto dataset = crossvi::vocabulary::activeDatasetInfo();
  reviewStoreReady_ = VOCABULARY_REVIEW.configure(dataset.identity, dataset.entryCount, dataset.external);

  SETTINGS.vocabularyDatasetPath[0] = '\0';
  if (external) copyDatasetPath(SETTINGS.vocabularyDatasetPath, path);
  if (!SETTINGS.saveToFile()) {
    copyDatasetPath(SETTINGS.vocabularyDatasetPath, previousPath.c_str());
    bool restored = true;
    if (previousPath.empty()) {
      crossvi::vocabulary::useBuiltInDataset();
    } else {
      restored = crossvi::vocabulary::useExternalDataset(previousPath.c_str());
    }
    if (!restored) crossvi::vocabulary::useBuiltInDataset();
    const auto previous = crossvi::vocabulary::activeDatasetInfo();
    reviewStoreReady_ = VOCABULARY_REVIEW.configure(previous.identity, previous.entryCount, previous.external);
    datasetLoadFailed_ = !restored;
    return false;
  }
  datasetLoadFailed_ = false;
  return true;
}

void VocabularyLearningActivity::handleQuestionInput() {
  const uint8_t seconds = configuredQuestionSeconds();
  if (seconds > 0) {
    const uint32_t elapsed = static_cast<uint32_t>(millis() - questionStartedAt_);
    const uint32_t duration = seconds * 1000UL;
    if (elapsed >= duration) {
      submitAnswer(-1, true);
      return;
    }
    const uint8_t segments = crossvi::vocabulary::countdownSegments(elapsed, duration);
    if (segments != countdownSegments_) {
      countdownSegments_ = segments;
      requestUpdate();
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) skipHold_.onPress();
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      skipHold_.onHold(mappedInput.getHeldTime(MappedInputManager::Button::Back), ReaderUtils::SKIP_HOLD_MS)) {
    finishActivity();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      skipHold_.onRelease() == ReaderUtils::HoldRelease::Short) {
    submitAnswer(-1);
    return;
  }
  navigator_.onNext([this] {
    selectedAnswer_ = ButtonNavigator::nextIndex(selectedAnswer_, answerCount_);
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    selectedAnswer_ = ButtonNavigator::previousIndex(selectedAnswer_, answerCount_);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) submitAnswer(selectedAnswer_);
}

void VocabularyLearningActivity::handleFeedbackInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    advanceAfterFeedback();
  }
}

void VocabularyLearningActivity::handleResultsInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    screen_ = Screen::Settings;
    requestUpdate();
    return;
  }
  navigator_.onNext([this] {
    selectedResultAction_ = ButtonNavigator::nextIndex(selectedResultAction_, 2);
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    selectedResultAction_ = ButtonNavigator::previousIndex(selectedResultAction_, 2);
    requestUpdate();
  });
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (selectedResultAction_ == 0) {
    reviewIndex_ = 0;
    screen_ = Screen::Review;
    requestUpdate();
  } else {
    if (reviewOnly_) {
      if (VOCABULARY_REVIEW.count() == 0) {
        screen_ = Screen::Settings;
        requestUpdate();
      } else {
        beginReviewQuiz();
      }
    } else {
      beginQuiz(questionCount_);
    }
  }
}

void VocabularyLearningActivity::handleReviewInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    screen_ = Screen::Results;
    requestUpdate();
    return;
  }
  navigator_.onNext([this] {
    reviewIndex_ = static_cast<uint8_t>(ButtonNavigator::nextIndex(reviewIndex_, questionCount_));
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    reviewIndex_ = static_cast<uint8_t>(ButtonNavigator::previousIndex(reviewIndex_, questionCount_));
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    reviewIndex_ = static_cast<uint8_t>(ButtonNavigator::nextIndex(reviewIndex_, questionCount_));
    requestUpdate();
  }
}

void VocabularyLearningActivity::beginQuiz(const uint8_t count) { startQuiz(count, false); }

void VocabularyLearningActivity::beginReviewQuiz() {
  if (!reviewStoreReady_) return;
  const size_t available = VOCABULARY_REVIEW.count();
  if (available == 0) return;

  const size_t target = std::min<size_t>(configuredQuestionCount(), available);
  size_t selected = 0;
  size_t seen = 0;
  for (size_t entryIndex = 0; entryIndex < crossvi::vocabulary::entryCount(); ++entryIndex) {
    if (!VOCABULARY_REVIEW.needsReview(entryIndex)) continue;
    ++seen;
    if (selected < target) {
      reviewEntryIndices_[selected++] = static_cast<uint16_t>(entryIndex);
      continue;
    }
    const size_t replacement = nextRandom(randomState_) % seen;
    if (replacement < target) reviewEntryIndices_[replacement] = static_cast<uint16_t>(entryIndex);
  }
  if (selected == 0) return;
  for (size_t index = selected - 1; index > 0; --index) {
    std::swap(reviewEntryIndices_[index], reviewEntryIndices_[nextRandom(randomState_) % (index + 1)]);
  }
  startQuiz(static_cast<uint8_t>(selected), true);
}

void VocabularyLearningActivity::startQuiz(const uint8_t count, const bool reviewOnly) {
  questionCount_ = std::clamp<uint8_t>(count, 1, MAX_QUESTIONS);
  reviewOnly_ = reviewOnly;
  reviewSaveFailed_ = false;
  answerCount_ = configuredAnswerCount();
  nextAnswerSlot_ = answerCount_;
  currentQuestion_ = 0;
  correctCount_ = 0;
  wrongCount_ = 0;
  skippedCount_ = 0;
  selectedResultAction_ = 0;
  records_.fill({});
  skipHold_.reset();
  prepareQuestion();
}

void VocabularyLearningActivity::prepareQuestion() {
  size_t entryIndex = reviewOnly_ ? reviewEntryIndices_[currentQuestion_] : 0;
  if (!reviewOnly_) {
    for (size_t attempt = 0; attempt < crossvi::vocabulary::entryCount(); ++attempt) {
      entryIndex = nextRandom(randomState_) % crossvi::vocabulary::entryCount();
      bool duplicate = false;
      for (uint8_t previous = 0; previous < currentQuestion_; ++previous) {
        if (records_[previous].entryIndex == entryIndex) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) break;
    }
  }
  QuestionRecord& record = records_[currentQuestion_];
  record.entryIndex = static_cast<uint16_t>(entryIndex);
  if (nextAnswerSlot_ >= answerCount_) {
    crossvi::vocabulary::buildAnswerSlotOrder(answerCount_, nextRandom(randomState_), answerSlotOrder_.data());
    nextAnswerSlot_ = 0;
  }
  record.correctSlot = answerSlotOrder_[nextAnswerSlot_++];
  size_t answers[crossvi::vocabulary::MAX_ANSWER_COUNT]{};
  if (!crossvi::vocabulary::buildAnswerIndices(entryIndex, record.correctSlot, answerCount_, nextRandom(randomState_),
                                               answers)) {
    LOG_ERR("VOCAB", "Vocabulary set cannot provide distinct answers");
    screen_ = Screen::Settings;
    datasetLoadFailed_ = true;
    requestUpdate();
    return;
  }
  for (size_t index = 0; index < answerCount_; ++index) {
    record.answerIndices[index] = static_cast<uint16_t>(answers[index]);
  }
  record.selectedSlot = -1;
  record.state = AnswerState::Skipped;
  selectedAnswer_ = 0;
  questionStartedAt_ = millis();
  countdownSegments_ = 5;
  skipHold_.reset();
  screen_ = Screen::Question;
  requestUpdate();
}

void VocabularyLearningActivity::submitAnswer(const int selectedSlot, const bool timedOut) {
  QuestionRecord& record = records_[currentQuestion_];
  record.selectedSlot = static_cast<int8_t>(selectedSlot);
  if (selectedSlot < 0) {
    record.state = timedOut ? AnswerState::TimedOut : AnswerState::Skipped;
    ++skippedCount_;
    if (timedOut && reviewStoreReady_) VOCABULARY_REVIEW.markForReview(record.entryIndex);
  } else if (selectedSlot == record.correctSlot) {
    record.state = AnswerState::Correct;
    ++correctCount_;
    if (reviewStoreReady_) VOCABULARY_REVIEW.markMastered(record.entryIndex);
  } else {
    record.state = AnswerState::Wrong;
    ++wrongCount_;
    if (reviewStoreReady_) VOCABULARY_REVIEW.markForReview(record.entryIndex);
  }
  screen_ = Screen::Feedback;
  requestUpdate();
}

void VocabularyLearningActivity::drawAnswerStateIcon(const AnswerState state, const int x, const int y,
                                                     const int size) const {
  if (state == AnswerState::Correct) {
    renderer.fillRoundedRect(x, y, size, size, 5, Color::Black);
    renderer.drawLine(x + size / 5, y + size / 2, x + size * 2 / 5, y + size * 3 / 4, 3, false);
    renderer.drawLine(x + size * 2 / 5, y + size * 3 / 4, x + size * 4 / 5, y + size / 4, 3, false);
    return;
  }

  renderer.drawRoundedRect(x, y, size, size, 2, 5, true);
  if (state == AnswerState::Skipped || state == AnswerState::TimedOut) {
    renderer.drawLine(x + size / 2, y + size / 5, x + size / 2, y + size * 3 / 5, 3, true);
    renderer.fillRect(x + size / 2 - 2, y + size * 3 / 4, 5, 5, true);
  } else {
    renderer.drawLine(x + size / 4, y + size / 4, x + size * 3 / 4, y + size * 3 / 4, 3, true);
    renderer.drawLine(x + size / 4, y + size * 3 / 4, x + size * 3 / 4, y + size / 4, 3, true);
  }
}

const char* VocabularyLearningActivity::answerStateLabel(const AnswerState state) const {
  switch (state) {
    case AnswerState::Correct:
      return tr(STR_VOCAB_CORRECT);
    case AnswerState::Wrong:
      return tr(STR_VOCAB_INCORRECT);
    case AnswerState::TimedOut:
      return tr(STR_VOCAB_TIME_UP);
    case AnswerState::Skipped:
      return tr(STR_VOCAB_SKIP);
  }
  return tr(STR_VOCAB_INCORRECT);
}

void VocabularyLearningActivity::advanceAfterFeedback() {
  if (currentQuestion_ + 1 < questionCount_) {
    ++currentQuestion_;
    prepareQuestion();
    return;
  }
  reviewSaveFailed_ = reviewStoreReady_ && !VOCABULARY_REVIEW.flush();
  screen_ = Screen::Results;
  requestUpdate();
}

void VocabularyLearningActivity::finishActivity() {
  if (reviewStoreReady_ && !VOCABULARY_REVIEW.flush()) LOG_ERR("VOCAB", "Failed to save words marked for review");
  ActivityResult result;
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void VocabularyLearningActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  switch (screen_) {
    case Screen::Settings:
      renderSettings();
      break;
    case Screen::Question:
      renderQuestion(false);
      break;
    case Screen::Feedback:
      renderQuestion(true);
      break;
    case Screen::Results:
      renderResults();
      break;
    case Screen::Review:
      renderReview();
      break;
    case Screen::Source:
      renderSource();
      break;
  }
  renderer.displayBuffer();
}

void VocabularyLearningActivity::renderSettings() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOCABULARY_LEARNING));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, 7, selectedSetting_,
      [](const int index) {
        constexpr StrId LABELS[] = {StrId::STR_VOCAB_START_QUIZ,    StrId::STR_VOCAB_REVIEW_WRONG,
                                    StrId::STR_VOCAB_SET,           StrId::STR_VOCAB_QUIZ_SIZE,
                                    StrId::STR_VOCAB_QUESTION_TIME, StrId::STR_VOCAB_ANSWER_COUNT,
                                    StrId::STR_VOCAB_DATA_SOURCE};
        return std::string(I18N.get(LABELS[index]));
      },
      nullptr, nullptr,
      [this](const int index) -> std::string {
        switch (index) {
          case 1:
            if (!reviewStoreReady_) return tr(STR_STATS_UNAVAILABLE);
            return std::to_string(VOCABULARY_REVIEW.count());
          case 2:
            if (datasetLoadFailed_) return tr(STR_FAILED_LOWER);
            return crossvi::vocabulary::activeDatasetInfo().title;
          case 3:
            return quizSizeLabel();
          case 4:
            return questionTimeLabel();
          case 5:
            return answerCountLabel();
          default:
            return {};
        }
      },
      false, [this](const int index) { return index == 1 && (!reviewStoreReady_ || VOCABULARY_REVIEW.count() == 0); });
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void VocabularyLearningActivity::drawAnswerCard(const int slot, const int x, const int y, const int width,
                                                const int height, const bool showFeedback) const {
  const QuestionRecord& record = records_[currentQuestion_];
  const bool correct = slot == record.correctSlot;
  const bool chosen = slot == record.selectedSlot;
  const bool selected = !showFeedback && slot == selectedAnswer_;
  const bool invert = showFeedback ? correct : selected;
  if (invert) {
    renderer.fillRoundedRect(x, y, width, height, 5, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, width, height, 1, 5, true);
  }
  const auto answer = crossvi::vocabulary::entryAt(record.answerIndices[slot]);
  const int textWidth = width - 28;
  const int answerFont = height < 42 ? SMALL_FONT_ID : UI_10_FONT_ID;
  const auto lines = renderer.wrappedText(answerFont, answer.meaning, textWidth, 2);
  const int lineHeight = renderer.getLineHeight(answerFont);
  const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
  int textY = y + std::max(4, (height - blockHeight) / 2);
  for (const auto& line : lines) {
    renderer.drawText(answerFont, x + 14, textY, line.c_str(), !invert);
    textY += lineHeight;
  }
  if (showFeedback && chosen && !correct) {
    renderer.drawText(SMALL_FONT_ID, x + width - 18, y + 5, "X", true, EpdFontFamily::BOLD);
  }
}

void VocabularyLearningActivity::renderQuestion(const bool showFeedback) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  char progress[32];
  snprintf(progress, sizeof(progress), tr(STR_VOCAB_QUESTION_FORMAT), static_cast<unsigned>(currentQuestion_ + 1),
           static_cast<unsigned>(questionCount_));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOCABULARY_LEARNING),
                 progress);

  const int x = 18;
  const bool compact = height < 650;
  const int contentTop = metrics.topPadding + metrics.headerHeight + 12;
  const int contentWidth = width - 36;
  const int railY = contentTop;
  renderer.drawRect(x, railY, contentWidth, 7);
  renderer.fillRect(x + 2, railY + 2,
                    std::max(1, (contentWidth - 4) * static_cast<int>(currentQuestion_ + 1) / questionCount_), 3, true);

  const auto word = crossvi::vocabulary::entryAt(records_[currentQuestion_].entryIndex);
  const int cardY = railY + 18;
  const int cardH = compact ? 84 : 120;
  renderer.drawRoundedRect(x, cardY, contentWidth, cardH, 1, 6, true);
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, cardY + (compact ? 5 : 6), word.word, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, cardY + (compact ? 40 : 58), word.pronunciation);
  if (!showFeedback) {
    const uint8_t seconds = configuredQuestionSeconds();
    if (seconds == 0) {
      renderer.drawCenteredText(SMALL_FONT_ID, cardY + (compact ? 64 : 94), tr(STR_VOCAB_UNLIMITED));
    } else {
      constexpr int SEGMENT_COUNT = 5;
      constexpr int SEGMENT_GAP = 5;
      constexpr int LABEL_GAP = 8;
      const std::string timerLabel = std::string(tr(STR_VOCAB_TIME_REMAINING)) + ":";
      const int timerY = cardY + (compact ? 60 : 92);
      const int labelX = x + 12;
      const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, timerLabel.c_str());
      renderer.drawText(SMALL_FONT_ID, labelX, timerY, timerLabel.c_str());
      const int segmentAreaX = labelX + labelWidth + LABEL_GAP;
      const int segmentAreaY = timerY + std::max(0, (renderer.getLineHeight(SMALL_FONT_ID) - 9) / 2);
      const int segmentAreaWidth = x + contentWidth - 12 - segmentAreaX;
      const int segmentWidth = (segmentAreaWidth - SEGMENT_GAP * (SEGMENT_COUNT - 1)) / SEGMENT_COUNT;
      for (int segment = 0; segment < SEGMENT_COUNT; ++segment) {
        const int segmentX = segmentAreaX + segment * (segmentWidth + SEGMENT_GAP);
        renderer.drawRoundedRect(segmentX, segmentAreaY, segmentWidth, 9, 1, 2, true);
        if (segment < countdownSegments_) {
          renderer.fillRoundedRect(segmentX + 2, segmentAreaY + 2, segmentWidth - 4, 5, 1, Color::Black);
        }
      }
    }
  }

  const int optionGap = compact ? (answerCount_ == 4 ? 4 : 6) : 8;
  const int optionH = compact ? (answerCount_ == 4 ? 38 : 44) : (answerCount_ == 4 ? 56 : 64);
  const int optionsY = cardY + cardH + (compact ? 8 : 12);
  for (int slot = 0; slot < answerCount_; ++slot) {
    drawAnswerCard(slot, x, optionsY + slot * (optionH + optionGap), contentWidth, optionH, showFeedback);
  }
  if (showFeedback) {
    const QuestionRecord& record = records_[currentQuestion_];
    const int feedbackY = optionsY + optionH * answerCount_ + optionGap * (answerCount_ - 1) + (compact ? 8 : 14);
    const int feedbackHeight = height - metrics.buttonHintsHeight - 16 - feedbackY;
    renderer.drawRoundedRect(x, feedbackY, contentWidth, feedbackHeight, 1, 6, true);

    const char* feedbackTitle = answerStateLabel(record.state);
    const int iconSize = compact ? 26 : 34;
    const int statusGap = compact ? 8 : 12;
    const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, feedbackTitle, EpdFontFamily::BOLD);
    const int statusWidth = iconSize + statusGap + titleWidth;
    const int iconX = x + std::max(10, (contentWidth - statusWidth) / 2);
    const int iconY = feedbackY + (compact ? 10 : 20);
    drawAnswerStateIcon(record.state, iconX, iconY, iconSize);
    const int titleY = iconY + std::max(0, (iconSize - renderer.getLineHeight(UI_12_FONT_ID)) / 2);
    renderer.drawText(UI_12_FONT_ID, iconX + iconSize + statusGap, titleY, feedbackTitle, true, EpdFontFamily::BOLD);

    const auto answer = crossvi::vocabulary::entryAt(record.answerIndices[record.correctSlot]);
    std::string answerText = std::string(tr(STR_VOCAB_CORRECT_ANSWER)) + ": " + answer.meaning;
    const auto answerLines =
        renderer.wrappedText(UI_10_FONT_ID, answerText.c_str(), contentWidth - 24, compact ? 1 : 2);
    int answerY = feedbackY + (compact ? 42 : 68);
    for (const auto& line : answerLines) {
      const int answerWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, x + std::max(12, (contentWidth - answerWidth) / 2), answerY, line.c_str());
      answerY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CONTINUE), tr(STR_CONTINUE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_VOCAB_SKIP), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void VocabularyLearningActivity::renderResults() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOCAB_RESULTS));
  char score[32];
  snprintf(score, sizeof(score), tr(STR_VOCAB_SCORE_FORMAT), static_cast<unsigned>(correctCount_),
           static_cast<unsigned>(questionCount_));
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, metrics.topPadding + metrics.headerHeight + 40, score, true,
                            EpdFontFamily::BOLD);

  const int cardY = metrics.topPadding + metrics.headerHeight + 104;
  const int cardW = (width - 52) / 3;
  constexpr StrId labels[] = {StrId::STR_VOCAB_CORRECT_COUNT_FORMAT, StrId::STR_VOCAB_WRONG_COUNT_FORMAT,
                              StrId::STR_VOCAB_SKIPPED_COUNT_FORMAT};
  const uint8_t values[] = {correctCount_, wrongCount_, skippedCount_};
  for (int index = 0; index < 3; ++index) {
    const int cardX = 16 + index * (cardW + 10);
    renderer.drawRoundedRect(cardX, cardY, cardW, 70, 1, 5, true);
    char value[28];
    snprintf(value, sizeof(value), I18N.get(labels[index]), static_cast<unsigned>(values[index]));
    const auto safe = renderer.truncatedText(SMALL_FONT_ID, value, cardW - 8);
    renderer.drawText(SMALL_FONT_ID, cardX + 6, cardY + 25, safe.c_str());
  }

  const int listY = cardY + 100;
  GUI.drawList(renderer, Rect{0, listY, width, 110}, 2, selectedResultAction_, [](const int index) {
    return std::string(I18N.get(index == 0 ? StrId::STR_VOCAB_REVIEW : StrId::STR_VOCAB_TRY_AGAIN));
  });
  if (reviewSaveFailed_) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  const auto labelsHint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labelsHint.btn1, labelsHint.btn2, labelsHint.btn3, labelsHint.btn4);
}

void VocabularyLearningActivity::renderReview() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const bool compact = renderer.getScreenHeight() < 650;
  char progress[32];
  snprintf(progress, sizeof(progress), tr(STR_VOCAB_REVIEW_FORMAT), static_cast<unsigned>(reviewIndex_ + 1),
           static_cast<unsigned>(questionCount_));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOCAB_REVIEW), progress);
  const QuestionRecord& record = records_[reviewIndex_];
  const auto word = crossvi::vocabulary::entryAt(record.entryIndex);
  const int top = metrics.topPadding + metrics.headerHeight + 28;
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, top, word.word, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 52, word.pronunciation);

  const int x = 20;
  const int boxW = width - 40;
  const int boxH = compact ? 104 : 124;
  const int firstY = top + 96;
  renderer.drawRoundedRect(x, firstY, boxW, boxH, 1, 5, true);
  const char* stateLabel = answerStateLabel(record.state);
  const int reviewIconSize = 26;
  const int reviewGap = 9;
  const int stateWidth = renderer.getTextWidth(UI_12_FONT_ID, stateLabel, EpdFontFamily::BOLD);
  const int statusX = x + std::max(12, (boxW - reviewIconSize - reviewGap - stateWidth) / 2);
  drawAnswerStateIcon(record.state, statusX, firstY + 16, reviewIconSize);
  renderer.drawText(UI_12_FONT_ID, statusX + reviewIconSize + reviewGap, firstY + 17, stateLabel, true,
                    EpdFontFamily::BOLD);
  const int chosenLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_VOCAB_YOUR_ANSWER));
  renderer.drawText(SMALL_FONT_ID, x + (boxW - chosenLabelWidth) / 2, firstY + 52, tr(STR_VOCAB_YOUR_ANSWER));
  const char* chosen = tr(STR_VOCAB_NOT_ANSWERED);
  if (record.selectedSlot >= 0) {
    chosen = crossvi::vocabulary::entryAt(record.answerIndices[record.selectedSlot]).meaning;
  }
  const auto chosenLines = renderer.wrappedText(UI_10_FONT_ID, chosen, boxW - 24, compact ? 1 : 2);
  int y = firstY + 78;
  for (const auto& line : chosenLines) {
    const int lineWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
    renderer.drawText(UI_10_FONT_ID, x + std::max(12, (boxW - lineWidth) / 2), y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  const int secondY = firstY + boxH + 16;
  renderer.fillRoundedRect(x, secondY, boxW, boxH, 5, Color::Black);
  const int correctLabelWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_VOCAB_CORRECT_ANSWER), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x + (boxW - correctLabelWidth) / 2, secondY + (compact ? 20 : 28),
                    tr(STR_VOCAB_CORRECT_ANSWER), false, EpdFontFamily::BOLD);
  const char* correct = crossvi::vocabulary::entryAt(record.answerIndices[record.correctSlot]).meaning;
  const auto correctLines = renderer.wrappedText(UI_10_FONT_ID, correct, boxW - 24, 2);
  y = secondY + (compact ? 50 : 60);
  for (const auto& line : correctLines) {
    const int lineWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
    renderer.drawText(UI_10_FONT_ID, x + std::max(12, (boxW - lineWidth) / 2), y, line.c_str(), false);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONTINUE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void VocabularyLearningActivity::renderSource() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOCAB_DATA_SOURCE));
  const int x = 20;
  const int textWidth = width - 40;
  int y = metrics.topPadding + metrics.headerHeight + 20;
  const auto dataset = crossvi::vocabulary::activeDatasetInfo();
  renderer.drawText(UI_12_FONT_ID, x, y, dataset.title, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 8;
  char entryCount[32];
  std::snprintf(entryCount, sizeof(entryCount), tr(STR_VOCAB_ENTRY_COUNT_FORMAT),
                static_cast<unsigned>(dataset.entryCount));
  renderer.drawText(SMALL_FONT_ID, x, y, entryCount);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 16;
  const StrId notice = dataset.external ? StrId::STR_VOCAB_EXTERNAL_SOURCE_NOTICE : StrId::STR_VOCAB_SOURCE_NOTICE;
  const auto noticeLines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(notice), textWidth, 4);
  for (const auto& line : noticeLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  if (!dataset.external) {
    y += 18;
    constexpr const char* SOURCES[] = {"https://english4u.com.vn/Uploads/files/3000.pdf",
                                       "https://github.com/thichhoc-org/thichhoc-dict",
                                       "https://creativecommons.org/licenses/by-sa/4.0/"};
    const char* paragraphs[] = {tr(STR_VOCAB_BUILT_IN_CREDITS), SOURCES[0], SOURCES[1], SOURCES[2]};
    for (const char* paragraph : paragraphs) {
      const auto lines = renderer.wrappedText(SMALL_FONT_ID, paragraph, textWidth, 5);
      for (const auto& line : lines) {
        if (y + renderer.getLineHeight(SMALL_FONT_ID) >= height - metrics.buttonHintsHeight - 8) break;
        renderer.drawText(SMALL_FONT_ID, x, y, line.c_str());
        y += renderer.getLineHeight(SMALL_FONT_ID);
      }
      y += 8;
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
