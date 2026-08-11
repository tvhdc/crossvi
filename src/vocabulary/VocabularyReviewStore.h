#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace crossvi::vocabulary {

class VocabularyReviewStore {
 public:
  static constexpr size_t WORD_COUNT = 3000;
  static constexpr size_t BITSET_BYTES = (WORD_COUNT + 7) / 8;

  static VocabularyReviewStore& getInstance();

  bool load();
  bool needsReview(size_t entryIndex);
  size_t count();
  bool markForReview(size_t entryIndex);
  bool markMastered(size_t entryIndex);
  bool flush();
  bool isWritable() const { return writable_; }

#ifdef UNIT_TEST
  void resetForTests() {
    loaded_ = false;
    writable_ = true;
    dirty_ = false;
    count_ = 0;
    bits_.fill(0);
  }
#endif

 private:
  static bool validate(const uint8_t* data, size_t size, void* context);
  bool set(size_t entryIndex, bool value);

  bool loaded_ = false;
  bool writable_ = true;
  bool dirty_ = false;
  size_t count_ = 0;
  std::array<uint8_t, BITSET_BYTES> bits_{};
};

}  // namespace crossvi::vocabulary

#define VOCABULARY_REVIEW crossvi::vocabulary::VocabularyReviewStore::getInstance()
