#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string_view>

#include "vocabulary/VocabularyData.h"
#include "vocabulary/VocabularyQuizTiming.h"

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

TEST(VocabularyData, BuildsThreeDistinctAnswersWithRequestedCorrectSlot) {
  const size_t correct = findWord("are").value();
  for (uint8_t slot = 0; slot < 3; ++slot) {
    size_t answers[3]{};
    crossvi::vocabulary::buildAnswerIndices(correct, slot, 0x12345678U + slot, answers);
    EXPECT_EQ(answers[slot], correct);
    EXPECT_NE(answers[0], answers[1]);
    EXPECT_NE(answers[0], answers[2]);
    EXPECT_NE(answers[1], answers[2]);
    EXPECT_STRNE(crossvi::vocabulary::entryAt(answers[(slot + 1) % 3]).meaning,
                 crossvi::vocabulary::entryAt(correct).meaning);
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
