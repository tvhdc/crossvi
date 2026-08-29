#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "DictionaryHistoryStore.h"
#include "StarDictSynonyms.h"

namespace {
constexpr const char* BASE = "/dictionaries/test/test";

void appendEntry(std::vector<uint8_t>& bytes, const std::string& alias, uint32_t ordinal) {
  bytes.insert(bytes.end(), alias.begin(), alias.end());
  bytes.push_back(0);
  bytes.push_back(static_cast<uint8_t>(ordinal >> 24));
  bytes.push_back(static_cast<uint8_t>(ordinal >> 16));
  bytes.push_back(static_cast<uint8_t>(ordinal >> 8));
  bytes.push_back(static_cast<uint8_t>(ordinal));
}

class DictionarySynonymTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    DICTIONARY_HISTORY.resetForTests();
  }
};
}  // namespace

TEST_F(DictionarySynonymTest, MissingSynonymFileNeedsNoSidecar) {
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
  EXPECT_TRUE(StarDictSynonyms::buildIndex(BASE));
}

TEST_F(DictionarySynonymTest, BuildsSampledIndexAndFindsAliasesAcrossSamples) {
  std::vector<uint8_t> synonyms;
  for (uint32_t index = 0; index < 300; ++index) {
    char alias[16];
    snprintf(alias, sizeof(alias), "alias%03lu", static_cast<unsigned long>(index));
    appendEntry(synonyms, alias, index * 3);
  }
  Storage.setFile(std::string(BASE) + ".syn", synonyms);

  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));
  EXPECT_TRUE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));

  uint32_t ordinal = 0;
  EXPECT_TRUE(StarDictSynonyms::lookupOrdinal(BASE, "alias000", ordinal));
  EXPECT_EQ(ordinal, 0U);
  EXPECT_TRUE(StarDictSynonyms::lookupOrdinal(BASE, "alias299", ordinal));
  EXPECT_EQ(ordinal, 897U);
  EXPECT_FALSE(StarDictSynonyms::lookupOrdinal(BASE, "alias404", ordinal));

  // Five header words plus offsets for entries 0 and 256.
  EXPECT_EQ(Storage.file(std::string(BASE) + ".qsyn").size(), 7U * sizeof(uint32_t));
}

TEST_F(DictionarySynonymTest, AChangedSourceInvalidatesTheSidecar) {
  std::vector<uint8_t> synonyms;
  appendEntry(synonyms, "mot", 1);
  Storage.setFile(std::string(BASE) + ".syn", synonyms);
  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));

  appendEntry(synonyms, "tu", 2);
  Storage.setFile(std::string(BASE) + ".syn", synonyms);
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));
}

TEST_F(DictionarySynonymTest, TruncatedOrExtendedSidecarIsRebuilt) {
  std::vector<uint8_t> synonyms;
  appendEntry(synonyms, "mot", 1);
  Storage.setFile(std::string(BASE) + ".syn", synonyms);
  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));

  auto sidecar = Storage.file(std::string(BASE) + ".qsyn");
  ASSERT_GT(sidecar.size(), 1u);
  sidecar.pop_back();
  Storage.setFile(std::string(BASE) + ".qsyn", sidecar);
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));

  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));
  sidecar = Storage.file(std::string(BASE) + ".qsyn");
  sidecar.push_back(0);
  Storage.setFile(std::string(BASE) + ".qsyn", sidecar);
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));
}

TEST_F(DictionarySynonymTest, CorruptSampleOffsetFallsBackToFullSynonymScan) {
  std::vector<uint8_t> synonyms;
  for (uint32_t index = 0; index < 300; ++index) {
    char alias[16];
    snprintf(alias, sizeof(alias), "alias%03lu", static_cast<unsigned long>(index));
    appendEntry(synonyms, alias, index);
  }
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));
  ASSERT_TRUE(StarDictSynonyms::buildIndex(BASE));

  const std::string sidecarPath = std::string(BASE) + ".qsyn";
  auto sidecar = Storage.file(sidecarPath);
  ASSERT_GE(sidecar.size(), 7U * sizeof(uint32_t));
  std::fill(sidecar.begin() + 6U * sizeof(uint32_t), sidecar.begin() + 7U * sizeof(uint32_t), 0xFFU);
  Storage.setFile(sidecarPath, std::move(sidecar));

  // The structurally valid cache is still considered current. Its bad sample
  // must not make the canonical .syn data permanently unsearchable.
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
  uint32_t ordinal = 0;
  EXPECT_TRUE(StarDictSynonyms::lookupOrdinal(BASE, "alias299", ordinal));
  EXPECT_EQ(ordinal, 299U);
}

TEST_F(DictionarySynonymTest, TruncatedEntryIsDisabledWithoutRepeatedRescan) {
  Storage.setFile(std::string(BASE) + ".syn", {'b', 'a', 'd', 0, 0, 0});

  EXPECT_FALSE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
  EXPECT_EQ(Storage.file(std::string(BASE) + ".qsyn").size(), 5U * sizeof(uint32_t));
  uint32_t ordinal = 0;
  EXPECT_FALSE(StarDictSynonyms::lookupOrdinal(BASE, "bad", ordinal));
}

TEST_F(DictionarySynonymTest, AliasLongerThanTheBoundIsRejected) {
  std::vector<uint8_t> synonyms(256, 'a');
  synonyms.push_back(0);
  synonyms.insert(synonyms.end(), {0, 0, 0, 1});
  Storage.setFile(std::string(BASE) + ".syn", synonyms);

  EXPECT_FALSE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
}

TEST_F(DictionarySynonymTest, SidecarWriteFailureRemainsRetryable) {
  std::vector<uint8_t> synonyms;
  for (uint32_t index = 0; index < 300; ++index) {
    char alias[16];
    snprintf(alias, sizeof(alias), "alias%03lu", static_cast<unsigned long>(index));
    appendEntry(synonyms, alias, index);
  }
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));

  // Placeholder, first sample, then the sample at entry 256.
  Storage.shortWriteOnCall(3);
  EXPECT_FALSE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));

  EXPECT_TRUE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
  uint32_t ordinal = 0;
  EXPECT_TRUE(StarDictSynonyms::lookupOrdinal(BASE, "alias299", ordinal));
  EXPECT_EQ(ordinal, 299U);
}

TEST_F(DictionarySynonymTest, SourceReadFailureRemainsRetryable) {
  std::vector<uint8_t> synonyms;
  appendEntry(synonyms, "alias", 7);
  const std::string sourcePath = std::string(BASE) + ".syn";
  Storage.setFile(sourcePath, std::move(synonyms));

  Storage.shortReadFor(sourcePath);
  EXPECT_FALSE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));

  EXPECT_TRUE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_FALSE(StarDictSynonyms::needsIndex(BASE));
}

TEST_F(DictionarySynonymTest, FinalHeaderWriteFailureClosesSidecarBeforeCleanup) {
  std::vector<uint8_t> synonyms;
  appendEntry(synonyms, "alias", 7);
  Storage.setFile(std::string(BASE) + ".syn", std::move(synonyms));

  // Placeholder, first sample, then the final header rewrite.
  Storage.shortWriteOnCall(3);
  EXPECT_FALSE(StarDictSynonyms::buildIndex(BASE));
  EXPECT_EQ(Storage.removeWhileOpenAttemptsFor(std::string(BASE) + ".qsyn"), 0U);
  EXPECT_TRUE(StarDictSynonyms::needsIndex(BASE));
}

TEST_F(DictionarySynonymTest, HistoryIsNormalizedDeduplicatedAndBounded) {
  DICTIONARY_HISTORY.record("  XÃ HỘI!  ");
  DICTIONARY_HISTORY.record("xã hội");
  for (int index = 0; index < 20; ++index) {
    DICTIONARY_HISTORY.record("word" + std::to_string(index));
  }
  ASSERT_TRUE(DICTIONARY_HISTORY.flush());
  ASSERT_EQ(DICTIONARY_HISTORY.entries().size(), DictionaryHistoryStore::MAX_ENTRIES);
  EXPECT_EQ(DICTIONARY_HISTORY.entries().front(), "word19");
  EXPECT_LE(Storage.file("/.crosspoint/dictionary_history.txt").size(), DictionaryHistoryStore::MAX_FILE_BYTES);

  DICTIONARY_HISTORY.resetForTests();
  ASSERT_TRUE(DICTIONARY_HISTORY.load());
  EXPECT_EQ(DICTIONARY_HISTORY.entries().size(), DictionaryHistoryStore::MAX_ENTRIES);
  EXPECT_EQ(DICTIONARY_HISTORY.entries().front(), "word19");
}

TEST_F(DictionarySynonymTest, InvalidHistoryRemainsReadOnlyAndIsNotOverwritten) {
  const std::vector<uint8_t> malformed = {'n', 'o', 't', '-', 'v', '1'};
  Storage.setFile("/.crosspoint/dictionary_history.txt", malformed);

  EXPECT_FALSE(DICTIONARY_HISTORY.load());
  EXPECT_FALSE(DICTIONARY_HISTORY.isWritable());
  DICTIONARY_HISTORY.record("safe");
  EXPECT_FALSE(DICTIONARY_HISTORY.flush());
  EXPECT_EQ(Storage.file("/.crosspoint/dictionary_history.txt"), malformed);
}

TEST_F(DictionarySynonymTest, AtomicWriteFailurePreservesPreviousHistory) {
  DICTIONARY_HISTORY.record("old");
  ASSERT_TRUE(DICTIONARY_HISTORY.flush());
  const auto original = Storage.file("/.crosspoint/dictionary_history.txt");

  DICTIONARY_HISTORY.record("new");
  Storage.shortWriteFor("/.crosspoint/dictionary_history.txt.tmp");
  EXPECT_FALSE(DICTIONARY_HISTORY.flush());
  EXPECT_EQ(Storage.file("/.crosspoint/dictionary_history.txt"), original);
}

TEST_F(DictionarySynonymTest, RepeatingTheMostRecentQueryDoesNotTouchStorage) {
  DICTIONARY_HISTORY.record("same");
  ASSERT_TRUE(DICTIONARY_HISTORY.flush());

  Storage.resetIoCounters();
  DICTIONARY_HISTORY.record("same");
  EXPECT_TRUE(DICTIONARY_HISTORY.flush());
  EXPECT_EQ(Storage.openReadAttemptsFor("/.crosspoint/dictionary_history.txt"), 0U);
}

TEST_F(DictionarySynonymTest, FailedClearRestoresHistoryInMemoryAndOnDisk) {
  DICTIONARY_HISTORY.record("old");
  ASSERT_TRUE(DICTIONARY_HISTORY.flush());
  const auto original = Storage.file("/.crosspoint/dictionary_history.txt");

  Storage.shortWriteFor("/.crosspoint/dictionary_history.txt.tmp");
  EXPECT_FALSE(DICTIONARY_HISTORY.clear());

  ASSERT_EQ(DICTIONARY_HISTORY.entries().size(), 1U);
  EXPECT_EQ(DICTIONARY_HISTORY.entries().front(), "old");
  EXPECT_EQ(Storage.file("/.crosspoint/dictionary_history.txt"), original);

  Storage.resetIoCounters();
  EXPECT_TRUE(DICTIONARY_HISTORY.flush());
  EXPECT_EQ(Storage.openReadAttemptsFor("/.crosspoint/dictionary_history.txt"), 0U);
}
