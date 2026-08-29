#include <HalStorage.h>
#include <InflateReader.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "DictZip.h"
#include "Dictionary.h"
#include "DictionaryRegistry.h"
#include "StarDictSynonyms.h"

namespace {
constexpr const char* BASE = "/dictionaries/test/test";

void appendBe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

void appendIndexEntry(std::vector<uint8_t>& bytes, const std::string& word, const uint32_t offset,
                      const uint32_t size) {
  bytes.insert(bytes.end(), word.begin(), word.end());
  bytes.push_back(0);
  appendBe32(bytes, offset);
  appendBe32(bytes, size);
}

void appendSynonym(std::vector<uint8_t>& bytes, const std::string& word, const uint32_t ordinal) {
  bytes.insert(bytes.end(), word.begin(), word.end());
  bytes.push_back(0);
  appendBe32(bytes, ordinal);
}

std::vector<uint8_t> validDictZip() {
  return {
      0x1f, 0x8b, 0x08, 0x04, 0, 0, 0, 0, 0, 0,  // gzip + FEXTRA
      12,   0,                                   // XLEN
      'R',  'A',  8,    0,                       // RA subfield
      1,    0,    4,    0,    1, 0, 1, 0,        // v1, chunk=4, one compressed byte
      0x00,                                      // compressed data (stubbed inflater)
      0,    0,    0,    0,                       // CRC32 (not inspected here)
      4,    0,    0,    0,                       // ISIZE
  };
}

class DictionaryLookupTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    ESP.setMaxAllocHeap(1024U * 1024U);
    InflateReaderStub::reset();
  }

  Dictionary openPlain(std::vector<uint8_t> index, std::vector<uint8_t> definitions) {
    Storage.setFile(std::string(BASE) + ".idx", std::move(index));
    Storage.setFile(std::string(BASE) + ".dict", std::move(definitions));
    Dictionary dictionary;
    EXPECT_TRUE(dictionary.open("test"));
    return dictionary;
  }
};
}  // namespace

namespace DictionaryRegistry {
bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || std::string(folderName) != "test") return false;
  basePathOut = BASE;
  return true;
}
}  // namespace DictionaryRegistry

TEST_F(DictionaryLookupTest, DistinguishesMissReadFailureAndEmptyDefinition) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  appendIndexEntry(index, "empty", 5, 0);
  Dictionary dictionary = openPlain(std::move(index), {'h', 'e', 'l', 'l', 'o'});

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::Found;
  EXPECT_FALSE(dictionary.lookupExact("missing", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);

  EXPECT_TRUE(dictionary.lookupExact("empty", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
  EXPECT_TRUE(definition.empty());

  Storage.shortReadFor(std::string(BASE) + ".dict");
  EXPECT_FALSE(dictionary.lookupExact("alpha", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  EXPECT_TRUE(definition.empty());
}

TEST_F(DictionaryLookupTest, TruncatedIndexIsAReadFailure) {
  Dictionary dictionary = openPlain({'a', 'l', 'p', 'h', 'a', 0, 0, 0}, {'x'});
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_FALSE(dictionary.lookup("alpha", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
}

TEST_F(DictionaryLookupTest, BuildIndexRejectsIncompleteEntry) {
  Dictionary dictionary = openPlain({'a', 'l', 'p', 'h', 'a', 0, 0, 0}, {'x'});

  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(Storage.exists((std::string(BASE) + ".qidx").c_str()));
}

TEST_F(DictionaryLookupTest, BuildIndexRejectsUnterminatedHeadword) {
  Dictionary dictionary = openPlain({'a', 'l', 'p', 'h', 'a'}, {'x'});

  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(Storage.exists((std::string(BASE) + ".qidx").c_str()));
}

TEST_F(DictionaryLookupTest, BuildIndexRejectsSourceReadErrorEvenWhenBytesAreEventuallyRead) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 1);
  Dictionary dictionary = openPlain(std::move(index), {'x'});
  Storage.shortReadFor(std::string(BASE) + ".idx");

  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(Storage.exists((std::string(BASE) + ".qidx").c_str()));
}

TEST_F(DictionaryLookupTest, ExactLookupStillSupportsPhrasesAndSynonyms) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  appendIndexEntry(index, "multi word", 5, 6);
  Dictionary dictionary = openPlain(std::move(index), {'f', 'i', 'r', 's', 't', 'p', 'h', 'r', 'a', 's', 'e'});
  std::vector<uint8_t> synonyms;
  appendSynonym(synonyms, "alias", 0);
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));
  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_TRUE(dictionary.lookupExact("alias", definition, headword, &result));
  EXPECT_EQ(definition, "first");
  EXPECT_EQ(headword, "alpha");
  EXPECT_TRUE(dictionary.lookupExact("multi word", definition, headword, &result));
  EXPECT_EQ(definition, "phrase");
}

TEST_F(DictionaryLookupTest, DictZipSeparatesMalformedShortReadAndInflateFailure) {
  DictZip::Info info;
  DictZip::ExtractError error = DictZip::ExtractError::None;
  HalFile input;

  Storage.setFile("/short.dz", {0x1f, 0x8b});
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/short.dz", input));
  EXPECT_FALSE(DictZip::parse(input, &info, &error));
  EXPECT_EQ(error, DictZip::ExtractError::ReadError);

  Storage.setFile("/malformed.dz", std::vector<uint8_t>(16, 0));
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/malformed.dz", input));
  EXPECT_FALSE(DictZip::parse(input, &info, &error));
  EXPECT_EQ(error, DictZip::ExtractError::Decompress);

  Storage.setFile("/inflate.dz", validDictZip());
  HalFile output = Storage.open("/output", O_WRITE | O_CREAT | O_TRUNC);
  InflateReaderStub::readSucceeds = false;
  EXPECT_FALSE(DictZip::extractEntry("/inflate.dz", 0, 1, output, &error));
  EXPECT_EQ(error, DictZip::ExtractError::Decompress);
}

TEST_F(DictionaryLookupTest, DictZipSeparatesLowMemoryAndOutputFailure) {
  Storage.setFile("/dictionary.dz", validDictZip());
  DictZip::ExtractError error = DictZip::ExtractError::None;

  HalFile output = Storage.open("/output", O_WRITE | O_CREAT | O_TRUNC);
  InflateReaderStub::initSucceeds = false;
  EXPECT_FALSE(DictZip::extractEntry("/dictionary.dz", 0, 1, output, &error));
  EXPECT_EQ(error, DictZip::ExtractError::LowMemory);

  InflateReaderStub::reset();
  output = Storage.open("/output", O_WRITE | O_CREAT | O_TRUNC);
  Storage.shortWriteFor("/output");
  EXPECT_FALSE(DictZip::extractEntry("/dictionary.dz", 0, 1, output, &error));
  EXPECT_EQ(error, DictZip::ExtractError::ReadError);
}

TEST_F(DictionaryLookupTest, DictZipStreamsCompressedChunks) {
  Storage.setFile("/dictionary.dz", validDictZip());
  HalFile output = Storage.open("/output", O_WRITE | O_CREAT | O_TRUNC);
  DictZip::ExtractError error = DictZip::ExtractError::None;

  ASSERT_TRUE(DictZip::extractEntry("/dictionary.dz", 0, 1, output, &error));
  EXPECT_TRUE(InflateReaderStub::callbackSet);
}

TEST_F(DictionaryLookupTest, DictionaryPropagatesDictZipFailure) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 1);
  Storage.setFile(std::string(BASE) + ".idx", std::move(index));
  Storage.setFile(std::string(BASE) + ".dict.dz", validDictZip());
  Dictionary dictionary;
  ASSERT_TRUE(dictionary.open("test"));

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  InflateReaderStub::readSucceeds = false;
  EXPECT_FALSE(dictionary.lookupExact("alpha", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Decompress);
}

TEST_F(DictionaryLookupTest, DictionaryRejectsFailedTemporaryExtractionClose) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 1);
  Storage.setFile(std::string(BASE) + ".idx", std::move(index));
  Storage.setFile(std::string(BASE) + ".dict.dz", validDictZip());
  Dictionary dictionary;
  ASSERT_TRUE(dictionary.open("test"));

  Storage.failCloseFor("/.crosspoint/dict.tmp");
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_FALSE(dictionary.lookupExact("alpha", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
}

TEST_F(DictionaryLookupTest, ReusesIndexFilesAcrossStemVariants) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  appendIndexEntry(index, "beta", 5, 4);
  Dictionary dictionary = openPlain(std::move(index), {'f', 'i', 'r', 's', 't', 'b', 'e', 't', 'a'});
  ASSERT_TRUE(dictionary.buildIndex());

  const std::string idxPath = std::string(BASE) + ".idx";
  const std::string qidxPath = std::string(BASE) + ".qidx";
  const size_t idxReadsBefore = Storage.openReadAttemptsFor(idxPath);
  const size_t qidxReadsBefore = Storage.openReadAttemptsFor(qidxPath);

  std::string definition;
  std::string headword;
  ASSERT_TRUE(dictionary.lookup("alphas", definition, headword));
  EXPECT_EQ(definition, "first");
  EXPECT_EQ(Storage.openReadAttemptsFor(idxPath), idxReadsBefore + 1);
  EXPECT_EQ(Storage.openReadAttemptsFor(qidxPath), qidxReadsBefore + 1);
}

TEST_F(DictionaryLookupTest, BuildIndexReportsFinalSyncAndCloseFailures) {
  const std::string qidxPath = std::string(BASE) + ".qidx";
  for (const bool failClose : {false, true}) {
    Storage.reset();
    std::vector<uint8_t> index;
    appendIndexEntry(index, "alpha", 0, 5);
    Dictionary dictionary = openPlain(std::move(index), {'h', 'e', 'l', 'l', 'o'});
    if (failClose) {
      Storage.failCloseFor(qidxPath);
    } else {
      Storage.failSyncOnce();
    }

    Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
    EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
    EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
    EXPECT_FALSE(Storage.exists(qidxPath.c_str()));
  }
}

TEST_F(DictionaryLookupTest, TruncatedOrExtendedQuickIndexIsRebuilt) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  Dictionary dictionary = openPlain(std::move(index), {'h', 'e', 'l', 'l', 'o'});
  ASSERT_TRUE(dictionary.buildIndex());

  const std::string qidxPath = std::string(BASE) + ".qidx";
  const std::vector<uint8_t> valid = Storage.file(qidxPath);
  ASSERT_GT(valid.size(), 1U);

  std::vector<uint8_t> truncated = valid;
  truncated.pop_back();
  Storage.setFile(qidxPath, std::move(truncated));
  EXPECT_TRUE(dictionary.needsIndex());

  std::vector<uint8_t> extended = valid;
  extended.push_back(0);
  Storage.setFile(qidxPath, std::move(extended));
  EXPECT_TRUE(dictionary.needsIndex());
}

TEST_F(DictionaryLookupTest, CorruptQuickIndexOffsetFallsBackToFullIndexScan) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  appendIndexEntry(index, "beta", 5, 4);
  Dictionary dictionary = openPlain(std::move(index), {'f', 'i', 'r', 's', 't', 'b', 'e', 't', 'a'});
  ASSERT_TRUE(dictionary.buildIndex());

  const std::string qidxPath = std::string(BASE) + ".qidx";
  auto qidx = Storage.file(qidxPath);
  ASSERT_GE(qidx.size(), 6U * sizeof(uint32_t));
  std::fill(qidx.begin() + 5U * sizeof(uint32_t), qidx.begin() + 6U * sizeof(uint32_t), 0xFFU);
  Storage.setFile(qidxPath, std::move(qidx));

  // The header still looks current, so lookup itself must reject the bad
  // sample offset and fall back to scanning the canonical .idx from byte 0.
  EXPECT_FALSE(dictionary.needsIndex());
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_TRUE(dictionary.lookupExact("alpha", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
  EXPECT_EQ(definition, "first");
}

TEST_F(DictionaryLookupTest, CorruptQuickIndexOrdinalOffsetFallsBackForSynonym) {
  std::vector<uint8_t> index;
  std::vector<uint8_t> definitions;
  for (uint32_t ordinal = 0; ordinal <= 512; ++ordinal) {
    char word[16];
    std::snprintf(word, sizeof(word), "word%03lu", static_cast<unsigned long>(ordinal));
    appendIndexEntry(index, word, static_cast<uint32_t>(definitions.size()), 1);
    definitions.push_back(static_cast<uint8_t>('a' + ordinal % 26));
  }
  Dictionary dictionary = openPlain(std::move(index), std::move(definitions));
  ASSERT_TRUE(dictionary.buildIndex());

  std::vector<uint8_t> synonyms;
  appendSynonym(synonyms, "alias", 512);
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));
  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));

  const std::string qidxPath = std::string(BASE) + ".qidx";
  auto qidx = Storage.file(qidxPath);
  const size_t thirdSampleOffset = 7U * sizeof(uint32_t);
  ASSERT_GE(qidx.size(), thirdSampleOffset + sizeof(uint32_t));
  std::fill(qidx.begin() + thirdSampleOffset, qidx.begin() + thirdSampleOffset + sizeof(uint32_t), 0xFFU);
  Storage.setFile(qidxPath, std::move(qidx));

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_TRUE(dictionary.lookupExact("alias", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
  EXPECT_EQ(headword, "word512");
  EXPECT_EQ(definition, "s");
}

TEST_F(DictionaryLookupTest, BuildingMissingSynonymSidecarKeepsValidMainIndex) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  Dictionary dictionary = openPlain(std::move(index), {'h', 'e', 'l', 'l', 'o'});
  ASSERT_TRUE(dictionary.buildIndex());

  std::vector<uint8_t> synonyms;
  appendSynonym(synonyms, "alias", 0);
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));
  ASSERT_TRUE(dictionary.needsIndex());

  const std::string qidxPath = std::string(BASE) + ".qidx";
  const std::vector<uint8_t> qidxBefore = Storage.file(qidxPath);
  Storage.resetIoCounters();

  ASSERT_TRUE(dictionary.buildIndex());
  EXPECT_EQ(Storage.openWriteAttemptsFor(qidxPath), 0U);
  EXPECT_EQ(Storage.file(qidxPath), qidxBefore);
  EXPECT_FALSE(dictionary.needsIndex());
}

TEST_F(DictionaryLookupTest, BuildingMissingSynonymSidecarReportsPublicationFailure) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "alpha", 0, 5);
  Dictionary dictionary = openPlain(std::move(index), {'h', 'e', 'l', 'l', 'o'});
  ASSERT_TRUE(dictionary.buildIndex());

  std::vector<uint8_t> synonyms;
  appendSynonym(synonyms, "alias", 0);
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));
  ASSERT_TRUE(dictionary.needsIndex());

  Storage.failSyncOnce();
  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_TRUE(dictionary.needsIndex());
}

TEST_F(DictionaryLookupTest, DoesNotApplyEnglishStemmingToUnicodeWords) {
  std::vector<uint8_t> index;
  appendIndexEntry(index, "\xE7\x8C\xAB", 0, 3);
  Dictionary dictionary = openPlain(std::move(index), {'c', 'a', 't'});

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::Found;
  EXPECT_FALSE(dictionary.lookup("\xE7\x8C\xABs", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
}
