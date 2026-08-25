#include "VocabularyData.h"

#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include "VocabularyData.generated.h"

namespace crossvi::vocabulary {
namespace {

constexpr std::array<uint8_t, 8> FILE_MAGIC = {'C', 'V', 'O', 'C', 'A', 'B', 0, 1};
constexpr uint16_t FILE_VERSION = 1;
constexpr size_t HEADER_SIZE = 96;
constexpr size_t INDEX_RECORD_SIZE = 16;
constexpr size_t MAX_EXTERNAL_ENTRIES = 10000;
constexpr size_t MAX_EXTERNAL_FILE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_RECORD_BYTES = Entry::WORD_CAPACITY + Entry::PRONUNCIATION_CAPACITY + Entry::MEANING_CAPACITY;

struct ExternalHeader {
  uint32_t entryCount = 0;
  uint32_t indexOffset = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  uint32_t contentCrc = 0;
  char title[DatasetInfo::TITLE_CAPACITY]{};
};

struct EntryMetadata {
  uint32_t dataOffset = 0;
  uint16_t dataLength = 0;
  PartOfSpeech partOfSpeech = PartOfSpeech::Other;
  uint64_t meaningHash = 0;
};

struct DatasetState {
  std::mutex mutex;
  HalFile file;
  ExternalHeader header;
  bool external = false;
};

DatasetState& state() {
  static DatasetState value;
  return value;
}

uint16_t readLe16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8U); }

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readLe64(const uint8_t* data) {
  return static_cast<uint64_t>(readLe32(data)) | (static_cast<uint64_t>(readLe32(data + 4)) << 32U);
}

uint64_t fnv1a64(const char* text) {
  uint64_t value = 14695981039346656037ULL;
  while (text && *text) {
    value ^= static_cast<uint8_t>(*text++);
    value *= 1099511628211ULL;
  }
  return value;
}

uint32_t updateCrc32(uint32_t value, const uint8_t* data, const size_t length) {
  value = ~value;
  for (size_t index = 0; index < length; ++index) {
    value ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) value = (value >> 1U) ^ ((value & 1U) ? 0xEDB88320U : 0U);
  }
  return ~value;
}

bool readExact(HalFile& file, void* destination, const size_t size) {
  return file.read(destination, size) == static_cast<int>(size);
}

bool closeAndFail(HalFile& file) {
  (void)file.close();
  return false;
}

bool readMetadata(HalFile& file, const ExternalHeader& header, const size_t index, EntryMetadata& metadata) {
  if (index >= header.entryCount) return false;
  std::array<uint8_t, INDEX_RECORD_SIZE> encoded{};
  const uint64_t offset = static_cast<uint64_t>(header.indexOffset) + index * INDEX_RECORD_SIZE;
  if (offset > SIZE_MAX || !file.seek(static_cast<size_t>(offset)) || !readExact(file, encoded.data(), encoded.size()))
    return false;
  metadata.dataOffset = readLe32(encoded.data());
  metadata.dataLength = readLe16(encoded.data() + 4);
  if (encoded[6] > static_cast<uint8_t>(PartOfSpeech::Other)) return false;
  metadata.partOfSpeech = static_cast<PartOfSpeech>(encoded[6]);
  metadata.meaningHash = readLe64(encoded.data() + 8);
  return metadata.dataLength >= 3 && metadata.dataLength <= MAX_RECORD_BYTES &&
         metadata.dataOffset <= header.dataSize && metadata.dataLength <= header.dataSize - metadata.dataOffset;
}

bool metadataAt(const size_t index, EntryMetadata& metadata) {
  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  if (dataset.external) return readMetadata(dataset.file, dataset.header, index, metadata);
  if (index >= generated::ENTRY_COUNT) return false;
  const char* word = generated::DATA + generated::OFFSETS[index];
  const char* pronunciation = word + std::strlen(word) + 1;
  const char* meaning = pronunciation + std::strlen(pronunciation) + 1;
  metadata.partOfSpeech = static_cast<PartOfSpeech>(generated::PARTS_OF_SPEECH[index]);
  metadata.meaningHash = fnv1a64(meaning);
  return true;
}

bool parseEntry(const uint8_t* data, const size_t length, const EntryMetadata& metadata, Entry& entry) {
  const uint8_t* end = data + length;
  const uint8_t* first = static_cast<const uint8_t*>(std::memchr(data, 0, length));
  if (!first || first == data) return false;
  const uint8_t* second = static_cast<const uint8_t*>(std::memchr(first + 1, 0, end - first - 1));
  if (!second) return false;
  const uint8_t* third = static_cast<const uint8_t*>(std::memchr(second + 1, 0, end - second - 1));
  if (!third || third != end - 1 || third == second + 1) return false;
  const size_t wordLength = first - data;
  const size_t pronunciationLength = second - first - 1;
  const size_t meaningLength = third - second - 1;
  if (wordLength >= Entry::WORD_CAPACITY || pronunciationLength >= Entry::PRONUNCIATION_CAPACITY ||
      meaningLength >= Entry::MEANING_CAPACITY)
    return false;
  std::memcpy(entry.word, data, wordLength);
  std::memcpy(entry.pronunciation, first + 1, pronunciationLength);
  std::memcpy(entry.meaning, second + 1, meaningLength);
  entry.partOfSpeech = metadata.partOfSpeech;
  return true;
}

Entry builtInEntry(const size_t index) {
  Entry entry;
  if (index >= generated::ENTRY_COUNT) return entry;
  const char* word = generated::DATA + generated::OFFSETS[index];
  const char* pronunciation = word + std::strlen(word) + 1;
  const char* meaning = pronunciation + std::strlen(pronunciation) + 1;
  if (std::strlen(word) >= Entry::WORD_CAPACITY || std::strlen(pronunciation) >= Entry::PRONUNCIATION_CAPACITY ||
      std::strlen(meaning) >= Entry::MEANING_CAPACITY)
    return entry;
  std::strcpy(entry.word, word);
  std::strcpy(entry.pronunciation, pronunciation);
  std::strcpy(entry.meaning, meaning);
  entry.partOfSpeech = static_cast<PartOfSpeech>(generated::PARTS_OF_SPEECH[index]);
  return entry;
}

uint32_t nextRandom(uint32_t& state) {
  if (state == 0) state = 0xA341316CU;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool isUsableDistractor(const size_t candidate, const size_t correct, const EntryMetadata& correctEntry,
                        const size_t* selected, const size_t selectedCount, const bool requireSamePart) {
  if (candidate == correct) return false;
  for (size_t index = 0; index < selectedCount; ++index) {
    if (selected[index] == candidate) return false;
  }
  EntryMetadata candidateEntry;
  if (!metadataAt(candidate, candidateEntry)) return false;
  if (requireSamePart && candidateEntry.partOfSpeech != correctEntry.partOfSpeech) return false;
  return candidateEntry.meaningHash != correctEntry.meaningHash;
}

size_t findDistractor(const size_t correct, const EntryMetadata& correctEntry, const size_t totalEntries,
                      const size_t* selected, const size_t selectedCount, uint32_t& seed) {
  // The common fast path keeps choices grammatically comparable. The bounded
  // fallback guarantees progress even for the small "Other" bucket.
  for (int pass = 0; pass < 2; ++pass) {
    const bool samePart = pass == 0;
    for (size_t attempt = 0; attempt < totalEntries; ++attempt) {
      const size_t candidate = nextRandom(seed) % totalEntries;
      if (isUsableDistractor(candidate, correct, correctEntry, selected, selectedCount, samePart)) return candidate;
    }
  }
  for (size_t candidate = 0; candidate < totalEntries; ++candidate) {
    if (isUsableDistractor(candidate, correct, correctEntry, selected, selectedCount, false)) return candidate;
  }
  return correct;
}

}  // namespace

size_t entryCount() {
  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  return dataset.external ? dataset.header.entryCount : generated::ENTRY_COUNT;
}

Entry entryAt(const size_t index) {
  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  if (!dataset.external) return builtInEntry(index);
  EntryMetadata metadata;
  Entry entry;
  std::array<uint8_t, MAX_RECORD_BYTES> data{};
  if (!readMetadata(dataset.file, dataset.header, index, metadata) ||
      !dataset.file.seek(dataset.header.dataOffset + metadata.dataOffset) ||
      !readExact(dataset.file, data.data(), metadata.dataLength) ||
      !parseEntry(data.data(), metadata.dataLength, metadata, entry))
    return {};
  return entry;
}

DatasetInfo activeDatasetInfo() {
  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  DatasetInfo info;
  info.external = dataset.external;
  info.entryCount = dataset.external ? dataset.header.entryCount : generated::ENTRY_COUNT;
  info.identity = dataset.external ? dataset.header.contentCrc : 0;
  const char* title = dataset.external ? dataset.header.title : "CrossVi English - Vietnamese";
  const size_t titleLength = std::min(std::strlen(title), sizeof(info.title) - 1);
  std::memcpy(info.title, title, titleLength);
  info.title[titleLength] = '\0';
  return info;
}

void useBuiltInDataset() {
  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  dataset.file.close();
  dataset.header = {};
  dataset.external = false;
}

bool useExternalDataset(const char* path) {
  if (!path || path[0] != '/' || std::strlen(path) >= 256) return false;
  HalFile file;
  if (!Storage.openFileForRead("VOCAB", path, file) || file.fileSize64() < HEADER_SIZE ||
      file.fileSize64() > MAX_EXTERNAL_FILE_BYTES)
    return false;

  std::array<uint8_t, HEADER_SIZE> encoded{};
  if (!readExact(file, encoded.data(), encoded.size()) ||
      !std::equal(FILE_MAGIC.begin(), FILE_MAGIC.end(), encoded.begin()) ||
      readLe16(encoded.data() + 8) != FILE_VERSION || readLe16(encoded.data() + 10) != HEADER_SIZE)
    return closeAndFail(file);

  ExternalHeader header;
  header.entryCount = readLe32(encoded.data() + 12);
  header.indexOffset = readLe32(encoded.data() + 16);
  header.dataOffset = readLe32(encoded.data() + 20);
  header.dataSize = readLe32(encoded.data() + 24);
  header.contentCrc = readLe32(encoded.data() + 28);
  const uint16_t titleLength = readLe16(encoded.data() + 32);
  if (header.entryCount < MAX_ANSWER_COUNT || header.entryCount > MAX_EXTERNAL_ENTRIES ||
      header.indexOffset != HEADER_SIZE || header.dataOffset != HEADER_SIZE + header.entryCount * INDEX_RECORD_SIZE ||
      static_cast<uint64_t>(header.dataOffset) + header.dataSize != file.fileSize64() ||
      titleLength >= DatasetInfo::TITLE_CAPACITY)
    return closeAndFail(file);
  std::memcpy(header.title, encoded.data() + 36, titleLength);
  if (header.title[0] == '\0') std::strcpy(header.title, "Custom vocabulary");

  uint32_t expectedDataOffset = 0;
  uint64_t distinctMeanings[MAX_ANSWER_COUNT]{};
  size_t distinctCount = 0;
  for (size_t index = 0; index < header.entryCount; ++index) {
    EntryMetadata metadata;
    if (!readMetadata(file, header, index, metadata) || metadata.dataOffset != expectedDataOffset)
      return closeAndFail(file);
    expectedDataOffset += metadata.dataLength;
    bool seen = false;
    for (size_t value = 0; value < distinctCount; ++value)
      seen = seen || distinctMeanings[value] == metadata.meaningHash;
    if (!seen && distinctCount < MAX_ANSWER_COUNT) distinctMeanings[distinctCount++] = metadata.meaningHash;
  }
  if (expectedDataOffset != header.dataSize || distinctCount < MAX_ANSWER_COUNT) return closeAndFail(file);

  if (!file.seek(header.indexOffset)) return closeAndFail(file);
  std::array<uint8_t, 1024> buffer{};
  uint32_t crc = 0;
  size_t remaining = static_cast<size_t>(file.fileSize64() - header.indexOffset);
  while (remaining > 0) {
    const size_t chunk = std::min(remaining, buffer.size());
    if (!readExact(file, buffer.data(), chunk)) return closeAndFail(file);
    crc = updateCrc32(crc, buffer.data(), chunk);
    remaining -= chunk;
  }
  if (crc != header.contentCrc) return closeAndFail(file);

  DatasetState& dataset = state();
  std::lock_guard lock(dataset.mutex);
  dataset.file.close();
  dataset.file = std::move(file);
  dataset.header = header;
  dataset.external = true;
  return true;
}

bool buildAnswerIndices(const size_t correctIndex, const uint8_t correctSlot, const uint8_t requestedAnswerCount,
                        uint32_t seed, size_t out[MAX_ANSWER_COUNT]) {
  const size_t totalEntries = entryCount();
  EntryMetadata correctEntry;
  if (!out || correctIndex >= totalEntries || !metadataAt(correctIndex, correctEntry)) return false;
  const uint8_t answerCount = std::clamp<uint8_t>(requestedAnswerCount, MIN_ANSWER_COUNT, MAX_ANSWER_COUNT);
  const size_t slot = correctSlot % answerCount;
  out[slot] = correctIndex;
  size_t selected[MAX_ANSWER_COUNT] = {correctIndex, totalEntries, totalEntries, totalEntries};
  size_t selectedCount = 1;
  for (size_t option = 0; option < answerCount; ++option) {
    if (option == slot) continue;
    const size_t distractor = findDistractor(correctIndex, correctEntry, totalEntries, selected, selectedCount, seed);
    if (distractor == correctIndex) return false;
    out[option] = distractor;
    selected[selectedCount++] = distractor;
  }
  return true;
}

void buildAnswerSlotOrder(const uint8_t requestedAnswerCount, uint32_t seed, uint8_t out[MAX_ANSWER_COUNT]) {
  if (!out) return;
  const uint8_t answerCount = std::clamp<uint8_t>(requestedAnswerCount, MIN_ANSWER_COUNT, MAX_ANSWER_COUNT);
  for (uint8_t slot = 0; slot < MAX_ANSWER_COUNT; ++slot) out[slot] = slot;
  for (uint8_t remaining = answerCount; remaining > 1; --remaining) {
    const uint8_t swapIndex = static_cast<uint8_t>(nextRandom(seed) % remaining);
    const uint8_t lastIndex = remaining - 1;
    const uint8_t previous = out[lastIndex];
    out[lastIndex] = out[swapIndex];
    out[swapIndex] = previous;
  }
}

}  // namespace crossvi::vocabulary
