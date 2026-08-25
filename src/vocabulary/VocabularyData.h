#pragma once

#include <cstddef>
#include <cstdint>

namespace crossvi::vocabulary {

enum class PartOfSpeech : uint8_t { Noun = 0, Verb = 1, Adjective = 2, Adverb = 3, Other = 4 };

struct Entry {
  static constexpr size_t WORD_CAPACITY = 97;
  static constexpr size_t PRONUNCIATION_CAPACITY = 97;
  static constexpr size_t MEANING_CAPACITY = 193;

  char word[WORD_CAPACITY]{};
  char pronunciation[PRONUNCIATION_CAPACITY]{};
  char meaning[MEANING_CAPACITY]{};
  PartOfSpeech partOfSpeech = PartOfSpeech::Other;
};

struct DatasetInfo {
  static constexpr size_t TITLE_CAPACITY = 61;

  char title[TITLE_CAPACITY]{};
  size_t entryCount = 0;
  uint32_t identity = 0;
  bool external = false;
};

inline constexpr uint8_t MIN_ANSWER_COUNT = 3;
inline constexpr uint8_t MAX_ANSWER_COUNT = 4;

size_t entryCount();
Entry entryAt(size_t index);
DatasetInfo activeDatasetInfo();
void useBuiltInDataset();
bool useExternalDataset(const char* path);

// Builds three or four distinct answer indices. The correct answer is placed
// at correctSlot; distractors prefer the same part of speech and always have a
// different meaning.
bool buildAnswerIndices(size_t correctIndex, uint8_t correctSlot, uint8_t answerCount, uint32_t seed,
                        size_t out[MAX_ANSWER_COUNT]);

// Shuffles every available answer position exactly once. Consuming a complete
// order before building the next one keeps correct answers varied without
// biasing any position.
void buildAnswerSlotOrder(uint8_t answerCount, uint32_t seed, uint8_t out[MAX_ANSWER_COUNT]);

}  // namespace crossvi::vocabulary
