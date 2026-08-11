#include "VocabularyReviewStore.h"

#include <AtomicFile.h>
#include <CredentialIntegrity.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>
#include <string_view>

namespace crossvi::vocabulary {
namespace {

constexpr char REVIEW_PATH[] = "/.crosspoint/vocabulary_review_v1.bin";
constexpr uint8_t HEADER[] = {'C',
                              'V',
                              'R',
                              'W',
                              1,
                              0,
                              static_cast<uint8_t>(VocabularyReviewStore::WORD_COUNT),
                              static_cast<uint8_t>(VocabularyReviewStore::WORD_COUNT >> 8U)};
constexpr size_t CRC_BYTES = sizeof(uint32_t);
constexpr size_t FILE_BYTES = sizeof(HEADER) + VocabularyReviewStore::BITSET_BYTES + CRC_BYTES;

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

void writeLe32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

bool isLoaded(const AtomicFile::LoadStatus status) {
  return status == AtomicFile::LoadStatus::Primary || status == AtomicFile::LoadStatus::Backup ||
         status == AtomicFile::LoadStatus::Temp;
}

}  // namespace

VocabularyReviewStore& VocabularyReviewStore::getInstance() {
  static VocabularyReviewStore instance;
  return instance;
}

bool VocabularyReviewStore::validate(const uint8_t* data, const size_t size, void*) {
  if (!data || size != FILE_BYTES || std::memcmp(data, HEADER, sizeof(HEADER)) != 0) return false;
  const std::string_view payload(reinterpret_cast<const char*>(data), FILE_BYTES - CRC_BYTES);
  return readLe32(data + FILE_BYTES - CRC_BYTES) == credential_integrity::crc32(payload);
}

bool VocabularyReviewStore::load() {
  if (loaded_) return writable_;
  loaded_ = true;
  bits_.fill(0);
  count_ = 0;

  std::string data;
  const auto status = AtomicFile::load(REVIEW_PATH, data, FILE_BYTES, validate);
  if (status == AtomicFile::LoadStatus::Missing) return true;
  if (!isLoaded(status)) {
    writable_ = false;
    LOG_ERR("VOCAB", "Review data is invalid or unreadable; preserving it read-only");
    return false;
  }

  std::memcpy(bits_.data(), data.data() + sizeof(HEADER), bits_.size());
  for (size_t entryIndex = 0; entryIndex < WORD_COUNT; ++entryIndex) {
    if ((bits_[entryIndex / 8] & (1U << (entryIndex % 8))) != 0) ++count_;
  }
  dirty_ = status != AtomicFile::LoadStatus::Primary;
  return true;
}

bool VocabularyReviewStore::needsReview(const size_t entryIndex) {
  load();
  return entryIndex < WORD_COUNT && (bits_[entryIndex / 8] & (1U << (entryIndex % 8))) != 0;
}

size_t VocabularyReviewStore::count() {
  load();
  return count_;
}

bool VocabularyReviewStore::set(const size_t entryIndex, const bool value) {
  load();
  if (!writable_ || entryIndex >= WORD_COUNT) return false;
  const uint8_t mask = static_cast<uint8_t>(1U << (entryIndex % 8));
  uint8_t& byte = bits_[entryIndex / 8];
  const bool current = (byte & mask) != 0;
  if (current == value) return true;
  if (value) {
    byte |= mask;
    ++count_;
  } else {
    byte &= static_cast<uint8_t>(~mask);
    --count_;
  }
  dirty_ = true;
  return true;
}

bool VocabularyReviewStore::markForReview(const size_t entryIndex) { return set(entryIndex, true); }

bool VocabularyReviewStore::markMastered(const size_t entryIndex) { return set(entryIndex, false); }

bool VocabularyReviewStore::flush() {
  load();
  if (!writable_) return false;
  if (!dirty_) return true;

  std::string data(FILE_BYTES, '\0');
  std::memcpy(data.data(), HEADER, sizeof(HEADER));
  std::memcpy(data.data() + sizeof(HEADER), bits_.data(), bits_.size());
  const auto payload = std::string_view(data.data(), FILE_BYTES - CRC_BYTES);
  writeLe32(reinterpret_cast<uint8_t*>(data.data()) + FILE_BYTES - CRC_BYTES, credential_integrity::crc32(payload));

  Storage.mkdir("/.crosspoint");
  const auto status = AtomicFile::save(REVIEW_PATH, data, FILE_BYTES, validate);
  if (status == AtomicFile::SaveStatus::Saved || status == AtomicFile::SaveStatus::Unchanged) {
    dirty_ = false;
    return true;
  }
  return false;
}

}  // namespace crossvi::vocabulary
