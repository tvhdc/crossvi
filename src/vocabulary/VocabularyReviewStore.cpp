#include "VocabularyReviewStore.h"

#include <AtomicFile.h>
#include <CredentialIntegrity.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace crossvi::vocabulary {
namespace {

constexpr char REVIEW_PATH[] = "/.crosspoint/vocabulary_review_v1.bin";
constexpr uint8_t V1_HEADER[] = {'C',
                                 'V',
                                 'R',
                                 'W',
                                 1,
                                 0,
                                 static_cast<uint8_t>(VocabularyReviewStore::BUILT_IN_WORD_COUNT),
                                 static_cast<uint8_t>(VocabularyReviewStore::BUILT_IN_WORD_COUNT >> 8U)};
constexpr uint8_t V2_MAGIC[] = {'C', 'V', 'R', 'W', 2, 0};
constexpr size_t V2_HEADER_BYTES = 16;
constexpr size_t CRC_BYTES = sizeof(uint32_t);
constexpr size_t BUILT_IN_BITSET_BYTES = (VocabularyReviewStore::BUILT_IN_WORD_COUNT + 7) / 8;
constexpr size_t V1_FILE_BYTES = sizeof(V1_HEADER) + BUILT_IN_BITSET_BYTES + CRC_BYTES;
constexpr size_t MAX_FILE_BYTES = V2_HEADER_BYTES + VocabularyReviewStore::BITSET_BYTES + CRC_BYTES;

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

uint16_t readLe16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8U); }

void writeLe16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
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
  if (!data || size < sizeof(V1_HEADER) + CRC_BYTES) return false;
  const std::string_view payload(reinterpret_cast<const char*>(data), size - CRC_BYTES);
  if (readLe32(data + size - CRC_BYTES) != credential_integrity::crc32(payload)) return false;
  if (size == V1_FILE_BYTES) return std::memcmp(data, V1_HEADER, sizeof(V1_HEADER)) == 0;
  if (size < V2_HEADER_BYTES + CRC_BYTES || std::memcmp(data, V2_MAGIC, sizeof(V2_MAGIC)) != 0) return false;
  const size_t wordCount = readLe16(data + 10);
  const size_t bitsetBytes = readLe16(data + 12);
  return wordCount >= 4 && wordCount <= MAX_WORD_COUNT && bitsetBytes == (wordCount + 7) / 8 &&
         size == V2_HEADER_BYTES + bitsetBytes + CRC_BYTES;
}

bool VocabularyReviewStore::configure(const uint32_t datasetIdentity, const size_t wordCount, const bool external) {
  if (wordCount < 4 || wordCount > MAX_WORD_COUNT) return false;
  if (loaded_ && dirty_ && !flush()) return false;
  loaded_ = false;
  writable_ = true;
  dirty_ = false;
  count_ = 0;
  bits_.fill(0);
  wordCount_ = wordCount;
  datasetIdentity_ = datasetIdentity;
  external_ = external;
  externalPath_[0] = '\0';
  if (external_)
    std::snprintf(externalPath_, sizeof(externalPath_), "/.crosspoint/vocabulary_review_%08lx_v2.bin",
                  static_cast<unsigned long>(datasetIdentity_));
  return load();
}

const char* VocabularyReviewStore::reviewPath() { return external_ ? externalPath_ : REVIEW_PATH; }

bool VocabularyReviewStore::load() {
  if (loaded_) return writable_;
  loaded_ = true;
  bits_.fill(0);
  count_ = 0;

  std::string data;
  const size_t expectedBytes = external_ ? V2_HEADER_BYTES + (wordCount_ + 7) / 8 + CRC_BYTES : V1_FILE_BYTES;
  const auto status = AtomicFile::load(reviewPath(), data, expectedBytes, validate);
  if (status == AtomicFile::LoadStatus::Missing) return true;
  if (!isLoaded(status)) {
    writable_ = false;
    LOG_ERR("VOCAB", "Review data is invalid or unreadable; preserving it read-only");
    return false;
  }

  const size_t payloadOffset = external_ ? V2_HEADER_BYTES : sizeof(V1_HEADER);
  if (external_ && (readLe32(reinterpret_cast<const uint8_t*>(data.data()) + 6) != datasetIdentity_ ||
                    readLe16(reinterpret_cast<const uint8_t*>(data.data()) + 10) != wordCount_)) {
    writable_ = false;
    LOG_ERR("VOCAB", "Review data belongs to a different vocabulary set; preserving it read-only");
    return false;
  }
  const size_t bitsetBytes = (wordCount_ + 7) / 8;
  std::memcpy(bits_.data(), data.data() + payloadOffset, bitsetBytes);
  for (size_t entryIndex = 0; entryIndex < wordCount_; ++entryIndex) {
    if ((bits_[entryIndex / 8] & (1U << (entryIndex % 8))) != 0) ++count_;
  }
  dirty_ = status != AtomicFile::LoadStatus::Primary;
  return true;
}

bool VocabularyReviewStore::needsReview(const size_t entryIndex) {
  load();
  return entryIndex < wordCount_ && (bits_[entryIndex / 8] & (1U << (entryIndex % 8))) != 0;
}

size_t VocabularyReviewStore::count() {
  load();
  return count_;
}

bool VocabularyReviewStore::set(const size_t entryIndex, const bool value) {
  load();
  if (!writable_ || entryIndex >= wordCount_) return false;
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

  const size_t bitsetBytes = (wordCount_ + 7) / 8;
  const size_t headerBytes = external_ ? V2_HEADER_BYTES : sizeof(V1_HEADER);
  const size_t fileBytes = headerBytes + bitsetBytes + CRC_BYTES;
  std::string data(fileBytes, '\0');
  if (external_) {
    std::memcpy(data.data(), V2_MAGIC, sizeof(V2_MAGIC));
    writeLe32(reinterpret_cast<uint8_t*>(data.data()) + 6, datasetIdentity_);
    writeLe16(reinterpret_cast<uint8_t*>(data.data()) + 10, static_cast<uint16_t>(wordCount_));
    writeLe16(reinterpret_cast<uint8_t*>(data.data()) + 12, static_cast<uint16_t>(bitsetBytes));
  } else {
    std::memcpy(data.data(), V1_HEADER, sizeof(V1_HEADER));
  }
  std::memcpy(data.data() + headerBytes, bits_.data(), bitsetBytes);
  const auto payload = std::string_view(data.data(), fileBytes - CRC_BYTES);
  writeLe32(reinterpret_cast<uint8_t*>(data.data()) + fileBytes - CRC_BYTES, credential_integrity::crc32(payload));

  Storage.mkdir("/.crosspoint");
  const auto status = AtomicFile::save(reviewPath(), data, fileBytes, validate);
  if (status == AtomicFile::SaveStatus::Saved || status == AtomicFile::SaveStatus::Unchanged) {
    dirty_ = false;
    return true;
  }
  return false;
}

}  // namespace crossvi::vocabulary
