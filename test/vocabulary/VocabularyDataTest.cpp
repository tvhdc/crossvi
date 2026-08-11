#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string_view>

#include "vocabulary/VocabularyData.h"
#include "vocabulary/VocabularyQuizTiming.h"
#include "vocabulary/VocabularyReviewStore.h"

namespace {

std::optional<size_t> findWord(const std::string_view word) {
  for (size_t index = 0; index < crossvi::vocabulary::entryCount(); ++index) {
    if (word == crossvi::vocabulary::entryAt(index).word) return index;
  }
  return std::nullopt;
}

}  // namespace

TEST(VocabularyData, ContainsExactlyThreeThousandCommonWords) {
  ASSERT_EQ(crossvi::vocabulary::entryCount(), 3000U);
  EXPECT_EQ(crossvi::vocabulary::entryCount(), crossvi::vocabulary::VocabularyReviewStore::WORD_COUNT);
  EXPECT_STREQ(crossvi::vocabulary::entryAt(0).word, "the");
  EXPECT_TRUE(findWord("are").has_value());
  EXPECT_TRUE(findWord("airport").has_value());
  EXPECT_TRUE(findWord("promise").has_value());

  // These names appeared when the unrestricted frequency list was used. The
  // production table must remain constrained to the common-word source list.
  EXPECT_FALSE(findWord("trump").has_value());
  EXPECT_FALSE(findWord("google").has_value());
  EXPECT_FALSE(findWord("twitter").has_value());
}

TEST(VocabularyData, EveryEntryHasBoundedDisplayData) {
  for (size_t index = 0; index < crossvi::vocabulary::entryCount(); ++index) {
    const auto entry = crossvi::vocabulary::entryAt(index);
    ASSERT_NE(entry.word, nullptr);
    ASSERT_NE(entry.pronunciation, nullptr);
    ASSERT_NE(entry.meaning, nullptr);
    EXPECT_GT(std::strlen(entry.word), 0U) << index;
    EXPECT_GT(std::strlen(entry.pronunciation), 0U) << entry.word;
    EXPECT_GT(std::strlen(entry.meaning), 0U) << entry.word;
    EXPECT_LE(std::strlen(entry.pronunciation), 48U) << entry.word;
    EXPECT_LE(std::strlen(entry.meaning), 92U) << entry.word;
  }
}

TEST(VocabularyData, BuildsThreeOrFourDistinctAnswersWithRequestedCorrectSlot) {
  const size_t correct = findWord("are").value();
  for (uint8_t answerCount = 3; answerCount <= 4; ++answerCount) {
    for (uint8_t slot = 0; slot < answerCount; ++slot) {
      size_t answers[crossvi::vocabulary::MAX_ANSWER_COUNT]{};
      crossvi::vocabulary::buildAnswerIndices(correct, slot, answerCount, 0x12345678U + slot, answers);
      EXPECT_EQ(answers[slot], correct);
      for (uint8_t first = 0; first < answerCount; ++first) {
        for (uint8_t second = first + 1; second < answerCount; ++second) {
          EXPECT_NE(answers[first], answers[second]);
          EXPECT_STRNE(crossvi::vocabulary::entryAt(answers[first]).meaning,
                       crossvi::vocabulary::entryAt(answers[second]).meaning);
        }
      }
    }
  }
}

TEST(VocabularyData, ShufflesEachCorrectAnswerPositionExactlyOncePerCycle) {
  for (uint8_t answerCount = 3; answerCount <= 4; ++answerCount) {
    for (uint32_t seed = 1; seed <= 16; ++seed) {
      uint8_t slots[crossvi::vocabulary::MAX_ANSWER_COUNT]{};
      crossvi::vocabulary::buildAnswerSlotOrder(answerCount, seed, slots);
      bool seen[crossvi::vocabulary::MAX_ANSWER_COUNT]{};
      for (uint8_t index = 0; index < answerCount; ++index) {
        ASSERT_LT(slots[index], answerCount);
        EXPECT_FALSE(seen[slots[index]]);
        seen[slots[index]] = true;
      }
      for (uint8_t slot = 0; slot < answerCount; ++slot) EXPECT_TRUE(seen[slot]);
    }
  }
}

TEST(VocabularyTiming, UsesFiveStableEinkFriendlyCountdownSteps) {
  using crossvi::vocabulary::countdownSegments;
  EXPECT_EQ(countdownSegments(0, 10000), 5);
  EXPECT_EQ(countdownSegments(1999, 10000), 5);
  EXPECT_EQ(countdownSegments(2000, 10000), 4);
  EXPECT_EQ(countdownSegments(8000, 10000), 1);
  EXPECT_EQ(countdownSegments(9999, 10000), 1);
  EXPECT_EQ(countdownSegments(10000, 10000), 0);
  EXPECT_EQ(countdownSegments(15000, 10000), 0);
  EXPECT_EQ(countdownSegments(5000, 0), 5);
}
