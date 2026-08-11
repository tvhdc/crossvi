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

size_t entryCount();
Entry entryAt(size_t index);

// Builds three distinct answer indices. The correct answer is placed at
// correctSlot; distractors prefer the same part of speech and always have a
// different meaning.
void buildAnswerIndices(size_t correctIndex, uint8_t correctSlot, uint32_t seed, size_t out[3]);

}  // namespace crossvi::vocabulary
