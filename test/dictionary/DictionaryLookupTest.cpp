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
