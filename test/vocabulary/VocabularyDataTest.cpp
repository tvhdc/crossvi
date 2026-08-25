#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vocabulary/VocabularyData.h"
#include "vocabulary/VocabularyQuizTiming.h"
#include "vocabulary/VocabularyReviewStore.h"

namespace {

struct TestEntry {
  std::string word;
  std::string pronunciation;
  std::string meaning;
  uint8_t partOfSpeech;
};

void appendLe16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLe32(std::vector<uint8_t>& out, const uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

void appendLe64(std::vector<uint8_t>& out, const uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

void writeLe32(std::vector<uint8_t>& out, const size_t offset, const uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
}

uint64_t fnv1a64(const std::string_view text) {
  uint64_t value = 14695981039346656037ULL;
  for (const unsigned char byte : text) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t value = UINT32_MAX;
  for (size_t index = 0; index < length; ++index) {
    value ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) value = (value >> 1U) ^ ((value & 1U) ? 0xEDB88320U : 0U);
  }
  return ~value;
}

std::vector<uint8_t> buildCvocab(const std::vector<TestEntry>& entries, const std::string_view title) {
  constexpr size_t headerSize = 96;
  constexpr size_t recordSize = 16;
  std::vector<uint8_t> index;
  std::vector<uint8_t> data;
  for (const TestEntry& entry : entries) {
    const uint32_t offset = static_cast<uint32_t>(data.size());
    data.insert(data.end(), entry.word.begin(), entry.word.end());
    data.push_back(0);
    data.insert(data.end(), entry.pronunciation.begin(), entry.pronunciation.end());
    data.push_back(0);
    data.insert(data.end(), entry.meaning.begin(), entry.meaning.end());
    data.push_back(0);
    appendLe32(index, offset);
    appendLe16(index, static_cast<uint16_t>(data.size() - offset));
    index.push_back(entry.partOfSpeech);
    index.push_back(0);
    appendLe64(index, fnv1a64(entry.meaning));
  }

  std::vector<uint8_t> result(headerSize, 0);
  const uint8_t magic[] = {'C', 'V', 'O', 'C', 'A', 'B', 0, 1};
  std::copy(std::begin(magic), std::end(magic), result.begin());
  result[8] = 1;
  result[10] = static_cast<uint8_t>(headerSize);
  writeLe32(result, 12, static_cast<uint32_t>(entries.size()));
  writeLe32(result, 16, headerSize);
  writeLe32(result, 20, static_cast<uint32_t>(headerSize + entries.size() * recordSize));
  writeLe32(result, 24, static_cast<uint32_t>(data.size()));
  result[32] = static_cast<uint8_t>(title.size());
  std::copy(title.begin(), title.end(), result.begin() + 36);
  result.insert(result.end(), index.begin(), index.end());
  result.insert(result.end(), data.begin(), data.end());
  writeLe32(result, 28, crc32(result.data() + headerSize, result.size() - headerSize));
  return result;
}

std::filesystem::path testStorageRoot() {
  const auto root = std::filesystem::temp_directory_path() / "crossvi-vocabulary-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  setenv("CROSSVI_SIM_SD", root.c_str(), 1);
  EXPECT_TRUE(Storage.begin());
  return root;
}

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

TEST(VocabularyData, LoadsValidatedExternalDatasetAndRestoresBuiltInData) {
  const auto root = testStorageRoot();
  const std::vector<TestEntry> entries = {{"bonjour", "bɔ̃.ʒuʁ", "hello", 4},
                                          {"livre", "livʁ", "book", 0},
                                          {"lire", "liʁ", "read", 1},
                                          {"vite", "vit", "quickly", 3}};
  const auto encoded = buildCvocab(entries, "French basics");
  std::ofstream(root / "french.cvocab", std::ios::binary)
      .write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));

  ASSERT_TRUE(crossvi::vocabulary::useExternalDataset("/french.cvocab"));
  const auto info = crossvi::vocabulary::activeDatasetInfo();
  EXPECT_TRUE(info.external);
  EXPECT_EQ(info.entryCount, entries.size());
  EXPECT_STREQ(info.title, "French basics");
  EXPECT_STREQ(crossvi::vocabulary::entryAt(1).word, "livre");
  EXPECT_STREQ(crossvi::vocabulary::entryAt(2).meaning, "read");

  size_t answers[crossvi::vocabulary::MAX_ANSWER_COUNT]{};
  EXPECT_TRUE(crossvi::vocabulary::buildAnswerIndices(0, 2, 4, 1234, answers));
  EXPECT_EQ(answers[2], 0U);

  crossvi::vocabulary::useBuiltInDataset();
  EXPECT_EQ(crossvi::vocabulary::entryCount(), 3000U);
  EXPECT_STREQ(crossvi::vocabulary::entryAt(0).word, "the");
}

TEST(VocabularyData, RejectsCorruptedOrAmbiguousExternalDataset) {
  const auto root = testStorageRoot();
  const std::vector<TestEntry> entries = {
      {"one", "", "same", 4}, {"two", "", "same", 4}, {"three", "", "same", 4}, {"four", "", "same", 4}};
  auto encoded = buildCvocab(entries, "Broken");
  std::ofstream(root / "ambiguous.cvocab", std::ios::binary)
      .write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
  EXPECT_FALSE(crossvi::vocabulary::useExternalDataset("/ambiguous.cvocab"));

  encoded = buildCvocab(
      {{"one", "", "first", 4}, {"two", "", "second", 4}, {"three", "", "third", 4}, {"four", "", "fourth", 4}},
      "Damaged");
  encoded.back() ^= 0x7F;
  std::ofstream(root / "damaged.cvocab", std::ios::binary)
      .write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
  EXPECT_FALSE(crossvi::vocabulary::useExternalDataset("/damaged.cvocab"));
  EXPECT_EQ(crossvi::vocabulary::entryCount(), 3000U);
}

TEST(VocabularyReviewStore, KeepsReviewProgressSeparateForEachExternalDataset) {
  const auto root = testStorageRoot();
  auto& store = crossvi::vocabulary::VocabularyReviewStore::getInstance();
  store.resetForTests();

  ASSERT_TRUE(store.configure(0x12345678U, 8, true));
  EXPECT_TRUE(store.markForReview(1));
  EXPECT_TRUE(store.markForReview(7));
  EXPECT_TRUE(store.flush());
  EXPECT_EQ(store.count(), 2U);

  ASSERT_TRUE(store.configure(0x87654321U, 8, true));
  EXPECT_EQ(store.count(), 0U);
  EXPECT_FALSE(store.needsReview(1));
  EXPECT_TRUE(store.markForReview(3));
  EXPECT_TRUE(store.flush());

  ASSERT_TRUE(store.configure(0x12345678U, 8, true));
  EXPECT_EQ(store.count(), 2U);
  EXPECT_TRUE(store.needsReview(1));
  EXPECT_TRUE(store.needsReview(7));
  EXPECT_FALSE(store.needsReview(3));

  EXPECT_TRUE(std::filesystem::exists(root / ".crosspoint/vocabulary_review_12345678_v2.bin"));
  EXPECT_TRUE(std::filesystem::exists(root / ".crosspoint/vocabulary_review_87654321_v2.bin"));
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
