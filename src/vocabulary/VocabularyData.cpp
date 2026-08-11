#include "VocabularyData.h"

#include <cstring>

#include "VocabularyData.generated.h"

namespace crossvi::vocabulary {
namespace {

uint32_t nextRandom(uint32_t& state) {
  if (state == 0) state = 0xA341316CU;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool isUsableDistractor(const size_t candidate, const size_t correct, const size_t* selected,
                        const size_t selectedCount, const bool requireSamePart) {
  if (candidate == correct) return false;
  for (size_t index = 0; index < selectedCount; ++index) {
    if (selected[index] == candidate) return false;
  }
  const Entry correctEntry = entryAt(correct);
  const Entry candidateEntry = entryAt(candidate);
  if (requireSamePart && candidateEntry.partOfSpeech != correctEntry.partOfSpeech) return false;
  return std::strcmp(candidateEntry.meaning, correctEntry.meaning) != 0;
}

size_t findDistractor(const size_t correct, const size_t* selected, const size_t selectedCount, uint32_t& seed) {
  // The common fast path keeps choices grammatically comparable. The bounded
  // fallback guarantees progress even for the small "Other" bucket.
  for (int pass = 0; pass < 2; ++pass) {
    const bool samePart = pass == 0;
    for (size_t attempt = 0; attempt < generated::ENTRY_COUNT; ++attempt) {
      const size_t candidate = nextRandom(seed) % generated::ENTRY_COUNT;
      if (isUsableDistractor(candidate, correct, selected, selectedCount, samePart)) return candidate;
    }
  }
  for (size_t candidate = 0; candidate < generated::ENTRY_COUNT; ++candidate) {
    if (isUsableDistractor(candidate, correct, selected, selectedCount, false)) return candidate;
  }
  return correct;
}

}  // namespace

size_t entryCount() { return generated::ENTRY_COUNT; }

Entry entryAt(const size_t index) {
  if (index >= generated::ENTRY_COUNT) return {};
  const char* word = generated::DATA + generated::OFFSETS[index];
  const char* pronunciation = word + std::strlen(word) + 1;
  const char* meaning = pronunciation + std::strlen(pronunciation) + 1;
  return {word, pronunciation, meaning, static_cast<PartOfSpeech>(generated::PARTS_OF_SPEECH[index])};
}

void buildAnswerIndices(const size_t correctIndex, const uint8_t correctSlot, uint32_t seed, size_t out[3]) {
  if (!out || correctIndex >= generated::ENTRY_COUNT) return;
  const size_t slot = correctSlot % 3;
  out[slot] = correctIndex;
  size_t selected[3] = {correctIndex, generated::ENTRY_COUNT, generated::ENTRY_COUNT};
  size_t selectedCount = 1;
  for (size_t option = 0; option < 3; ++option) {
    if (option == slot) continue;
    const size_t distractor = findDistractor(correctIndex, selected, selectedCount, seed);
    out[option] = distractor;
    selected[selectedCount++] = distractor;
  }
}

}  // namespace crossvi::vocabulary
