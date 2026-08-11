#pragma once

#include <cstddef>
#include <cstdint>

namespace crossvi::vocabulary {

enum class PartOfSpeech : uint8_t { Noun = 0, Verb = 1, Adjective = 2, Adverb = 3, Other = 4 };

struct Entry {
  const char* word = "";
  const char* pronunciation = "";
  const char* meaning = "";
  PartOfSpeech partOfSpeech = PartOfSpeech::Other;
};

inline constexpr uint8_t MIN_ANSWER_COUNT = 3;
inline constexpr uint8_t MAX_ANSWER_COUNT = 4;

size_t entryCount();
Entry entryAt(size_t index);

// Builds three or four distinct answer indices. The correct answer is placed
// at correctSlot; distractors prefer the same part of speech and always have a
// different meaning.
void buildAnswerIndices(size_t correctIndex, uint8_t correctSlot, uint8_t answerCount, uint32_t seed,
                        size_t out[MAX_ANSWER_COUNT]);

// Shuffles every available answer position exactly once. Consuming a complete
// order before building the next one keeps correct answers varied without
// biasing any position.
void buildAnswerSlotOrder(uint8_t answerCount, uint32_t seed, uint8_t out[MAX_ANSWER_COUNT]);

}  // namespace crossvi::vocabulary
