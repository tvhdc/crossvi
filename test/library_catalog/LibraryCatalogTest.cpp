#include <Epub.h>
#include <HalStorage.h>
#include <Xtc.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryCatalogStore.h"

namespace {
namespace fs = std::filesystem;

class LibraryCatalogTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* testInfo = testing::UnitTest::GetInstance()->current_test_info();
    root_ =
        fs::temp_directory_path() / ("crossvi-library-catalog-" + std::to_string(getpid()) + "-" + testInfo->name());
    fs::remove_all(root_);
    fs::create_directories(root_);
    ASSERT_EQ(setenv("CROSSVI_SIM_SD", root_.c_str(), 1), 0);
    ASSERT_TRUE(Storage.begin());
    LIBRARY_CATALOG.cancel();
    LIBRARY_CATALOG.invalidateSourceValidation();
    Epub::resetMetadata();
    Xtc::resetMetadata();
  }

  void TearDown() override {
    LIBRARY_CATALOG.cancel();
    fs::remove_all(root_);
  }

  void addBook(const std::string& name) {
    std::ofstream file(root_ / name, std::ios::binary);
    file << "book";
    ASSERT_TRUE(file.good());
  }

  void advanceToSorting() {
    for (size_t steps = 0; steps < 1000 && LIBRARY_CATALOG.phase() != LibraryCatalogStore::Phase::Sorting; ++steps) {
      LIBRARY_CATALOG.step();
    }
    ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Sorting);
  }

  void advanceToReady() {
    for (size_t steps = 0; steps < 2000 && !LIBRARY_CATALOG.isReady(); ++steps) LIBRARY_CATALOG.step();
    ASSERT_TRUE(LIBRARY_CATALOG.isReady());
  }

  bool loadOrderedIndices(const uint8_t sortMode, std::vector<size_t>& indices) {
    for (size_t steps = 0; steps < 20000; ++steps) {
      if (LIBRARY_CATALOG.loadOrderedIndices(sortMode, static_cast<LibraryBookFormat>(0xFF), indices)) return true;
      if (!LIBRARY_CATALOG.isOrderBuilding()) return false;
      LIBRARY_CATALOG.step();
    }
    return false;
  }

  fs::path root_;
};

TEST_F(LibraryCatalogTest, FinalizeCopiesOneRecordPerStep) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToSorting();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 3U);

  const fs::path temporary = root_ / ".crosspoint/library.idx.tmp";
  std::vector<uintmax_t> sizes;
  for (size_t record = 0; record < 3; ++record) {
    LIBRARY_CATALOG.step();
    ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Sorting);
    ASSERT_TRUE(fs::exists(temporary));
    sizes.push_back(fs::file_size(temporary));
  }
  ASSERT_EQ(sizes.size(), 3U);
  EXPECT_GT(sizes[0], 0U);
  EXPECT_EQ(sizes[1] - sizes[0], sizes[2] - sizes[1]);

  // Rotation is constant-size work; read-back validation then checks one
  // record per subsequent step while the previous catalog stays recoverable.
  LIBRARY_CATALOG.step();
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Sorting);
  EXPECT_FALSE(fs::exists(temporary));
  for (size_t record = 0; record < 3; ++record) {
    LIBRARY_CATALOG.step();
    EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Sorting);
  }
  LIBRARY_CATALOG.step();
  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
}

TEST_F(LibraryCatalogTest, CancelDuringFinalizeRestoresPrimaryState) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord original;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, original));

  addBook("new-one.txt");
  addBook("new-two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToSorting();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 3U);
  LIBRARY_CATALOG.step();
  LIBRARY_CATALOG.cancel();

  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord restored;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, restored));
  EXPECT_EQ(restored.path, original.path);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.idx.tmp"));
  EXPECT_FALSE(LIBRARY_CATALOG.consumeLastBuildFailed());
}

TEST_F(LibraryCatalogTest, CancelDuringCooperativePublishRollsBackToPrimaryCatalog) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);

  addBook("new.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToSorting();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 2U);
  LIBRARY_CATALOG.step();
  LIBRARY_CATALOG.step();
  LIBRARY_CATALOG.step();  // rotate; read-back validation remains pending
  ASSERT_TRUE(fs::exists(root_ / ".crosspoint/library.idx.bak"));

  LIBRARY_CATALOG.cancel();
  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.idx.bak"));
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.path, "/original.txt");
}

TEST_F(LibraryCatalogTest, CancelDuringDiscoveryRestoresPrimaryState) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);

  addBook("new.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Discovering);
  LIBRARY_CATALOG.cancel();

  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
}

TEST_F(LibraryCatalogTest, CorruptFinalRecordNeverReplacesPrimary) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();
  LibraryBookRecord original;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, original));

  addBook("new.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  EXPECT_NE(LIBRARY_CATALOG.generation(), originalGeneration);
  advanceToSorting();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 2U);
  LIBRARY_CATALOG.step();
  LIBRARY_CATALOG.step();

  const fs::path temporary = root_ / ".crosspoint/library.idx.tmp";
  ASSERT_TRUE(fs::exists(temporary));
  std::fstream file(temporary, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char byte = 0;
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte ^= 0x01;
  file.seekp(-1, std::ios::end);
  file.write(&byte, 1);
  file.close();

  advanceToReady();
  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.generation(), originalGeneration);
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord restored;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, restored));
  EXPECT_EQ(restored.path, original.path);
  EXPECT_FALSE(fs::exists(temporary));
  EXPECT_TRUE(LIBRARY_CATALOG.consumeLastBuildFailed());
  EXPECT_FALSE(LIBRARY_CATALOG.consumeLastBuildFailed());
}

TEST_F(LibraryCatalogTest, OpenDiscardsStaleTemporaryAndKeepsPrimary) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);

  const fs::path temporary = root_ / ".crosspoint/library.idx.tmp";
  {
    std::ofstream file(temporary, std::ios::binary);
    file << "partial";
  }
  ASSERT_TRUE(fs::exists(temporary));
  EXPECT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  EXPECT_FALSE(fs::exists(temporary));
}

TEST_F(LibraryCatalogTest, EmptyCatalogRediscoversFirstBookCopiedDirectlyToSd) {
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 0U);

  addBook("copied.epub");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Discovering);
  advanceToReady();

  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.path, "/copied.epub");
}

TEST_F(LibraryCatalogTest, BatchLoadReturnsRequestedContiguousRecords) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(1, 2, records));
  ASSERT_EQ(records.size(), 2U);
  EXPECT_NE(records[0].path, records[1].path);
}

TEST_F(LibraryCatalogTest, IncrementalUpdateTruncatesMetadataAndDropsOversizedDerivedCover) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);

  addBook("new.epub");
  std::string longTitle = "x";
  for (size_t index = 0; index < 100; ++index) longTitle += "ế";
  const std::string longAuthor(300, 'a');
  Epub::setMetadata({longTitle, longAuthor, "cover.jpg"}, std::string(LibraryCatalogStore::MAX_PATH_BYTES + 1, 'c'));
  LibraryCatalogStore::markDirtyPath("/new.epub");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Updating);
  advanceToReady();
  ASSERT_EQ(LIBRARY_CATALOG.count(), 2U);

  size_t index = 0;
  ASSERT_EQ(LIBRARY_CATALOG.findPath("/new.epub", 0, index), LibraryCatalogStore::FindPathResult::Found);
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(index, record));
  EXPECT_EQ(record.title.size(), 253U);
  EXPECT_EQ(record.title, longTitle.substr(0, 253));
  EXPECT_EQ(record.author.size(), LibraryCatalogStore::MAX_AUTHOR_BYTES);
  EXPECT_TRUE(record.coverBmpPath.empty());
}

TEST_F(LibraryCatalogTest, XtcEnrichmentUsesMetadataProbeWithoutOpeningTheWholeBook) {
  addBook("metadata.xtc");
  Xtc::setMetadata("Metadata title", "Metadata author", "/.crosspoint/xtc/thumb.bmp");

  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  ASSERT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.title, "Metadata title");
  EXPECT_EQ(record.author, "Metadata author");
  EXPECT_EQ(record.coverBmpPath, "/.crosspoint/xtc/thumb.bmp");
  EXPECT_EQ(Xtc::metadataReadCalls(), 1U);
  EXPECT_EQ(Xtc::loadCalls(), 0U);
}

TEST_F(LibraryCatalogTest, EpubDiscoveryAndMetadataEnrichmentRunInSeparateBoundedSteps) {
  addBook("metadata.epub");
  Epub::setMetadata({"Metadata title", "Metadata author", "cover.jpg"}, "/.crosspoint/epub/thumb.bmp", 3);

  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  for (size_t steps = 0; steps < 1000 && LIBRARY_CATALOG.phase() == LibraryCatalogStore::Phase::Discovering; ++steps) {
    LIBRARY_CATALOG.step();
  }
  ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Enriching);
  EXPECT_EQ(Epub::metadataBeginCalls(), 0U);
  EXPECT_EQ(Epub::metadataStepCalls(), 0U);

  // One catalog step only starts the job; subsequent steps pump exactly one
  // bounded metadata chunk instead of parsing the whole EPUB during discovery.
  LIBRARY_CATALOG.step();
  EXPECT_EQ(Epub::metadataBeginCalls(), 1U);
  EXPECT_EQ(Epub::metadataStepCalls(), 0U);
  for (size_t step = 1; step <= 3; ++step) {
    LIBRARY_CATALOG.step();
    EXPECT_EQ(Epub::metadataStepCalls(), step);
    EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Enriching);
  }
  LIBRARY_CATALOG.step();
  EXPECT_EQ(Epub::metadataStepCalls(), 4U);

  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.title, "Metadata title");
  EXPECT_EQ(record.author, "Metadata author");
  EXPECT_EQ(record.coverBmpPath, "/.crosspoint/epub/thumb.bmp");
}

TEST_F(LibraryCatalogTest, OversizedSourcePathKeepsCommittedCatalogAndDirtyMarker) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  fs::path directory = root_;
  for (size_t component = 0; component < 5; ++component) {
    directory /= std::string(90, static_cast<char>('a' + component));
  }
  ASSERT_TRUE(fs::create_directories(directory));
  {
    std::ofstream file(directory / (std::string(80, 'z') + ".epub"), std::ios::binary);
    file << "book";
    ASSERT_TRUE(file.good());
  }

  LibraryCatalogStore::markDirty();
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  advanceToReady();

  EXPECT_TRUE(LIBRARY_CATALOG.consumeLastBuildFailed());
  EXPECT_EQ(LIBRARY_CATALOG.generation(), originalGeneration);
  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.path, "/original.txt");
  EXPECT_TRUE(fs::exists(root_ / ".crosspoint/library.dirty"));
}

TEST_F(LibraryCatalogTest, OrderedIndicesUsePersistedSortOrder) {
  addBook("Zulu.txt");
  addBook("alpha.txt");
  addBook("Middle.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> indices;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, indices));
  ASSERT_EQ(indices.size(), 3U);
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(indices, records));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].title, "alpha");
  EXPECT_EQ(records[1].title, "Middle");
  EXPECT_EQ(records[2].title, "Zulu");
}

TEST_F(LibraryCatalogTest, FoldedSortKeysPreserveOriginalValueAndEmptyAuthorTieBreaks) {
  addBook("alpha.txt");
  addBook("Álpha.txt");
  addBook("punctuation.epub");
  Epub::setMetadata({"Punctuation", "---", ""}, "");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> indices;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, indices));
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(indices, records));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].title, "alpha");
  EXPECT_EQ(records[1].title, "Álpha");

  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_AUTHOR_ASC, indices));
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(indices, records));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].author, "---");
  EXPECT_TRUE(records[1].author.empty());
  EXPECT_TRUE(records[2].author.empty());
}

TEST_F(LibraryCatalogTest, OptimizedOrderPreservesDateAuthorAndLongPathTieBreaks) {
  const std::string directoryName(70, 'd');
  ASSERT_TRUE(fs::create_directories(root_ / directoryName));
  const fs::path firstPath = root_ / directoryName / "a.epub";
  const fs::path secondPath = root_ / directoryName / "z.epub";
  {
    std::ofstream first(firstPath, std::ios::binary);
    std::ofstream second(secondPath, std::ios::binary);
    first << "first";
    second << "second";
    ASSERT_TRUE(first.good());
    ASSERT_TRUE(second.good());
  }
  addBook("without-author.txt");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(firstPath, now - std::chrono::hours(2));
  fs::last_write_time(secondPath, now - std::chrono::hours(1));
  fs::last_write_time(root_ / "without-author.txt", now - std::chrono::hours(3));
  Epub::setMetadata({"Shared title", "Shared author", ""}, "");

  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> indices;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_DATE_ADDED_DESC, indices));
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(indices, records));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].path, "/" + directoryName + "/z.epub");
  EXPECT_EQ(records[1].path, "/" + directoryName + "/a.epub");
  EXPECT_EQ(records[2].path, "/without-author.txt");

  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_AUTHOR_ASC, indices));
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(indices, records));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].path, "/" + directoryName + "/a.epub");
  EXPECT_EQ(records[1].path, "/" + directoryName + "/z.epub");
  EXPECT_EQ(records[2].path, "/without-author.txt");
}

TEST_F(LibraryCatalogTest, OrderSidecarBuildsCooperativelyWithoutReplacingCatalog) {
  addBook("Zulu.txt");
  addBook("alpha.txt");
  addBook("Middle.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t generation = LIBRARY_CATALOG.generation();

  std::vector<size_t> indices;
  EXPECT_FALSE(LIBRARY_CATALOG.loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC,
                                                  static_cast<LibraryBookFormat>(0xFF), indices));
  EXPECT_TRUE(LIBRARY_CATALOG.isOrderBuilding());
  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.generation(), generation);
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, indices));
  EXPECT_EQ(indices.size(), 3U);
}

TEST_F(LibraryCatalogTest, OrderedIndicesPersistAcrossCatalogReopen) {
  addBook("Zulu.txt");
  addBook("alpha.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> first;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, first));
  ASSERT_TRUE(fs::exists(root_ / ".crosspoint/library.order"));

  LIBRARY_CATALOG.cancel();
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  std::vector<size_t> reopened;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, reopened));
  EXPECT_EQ(reopened, first);
}

TEST_F(LibraryCatalogTest, OrderSidecarFiltersFormatsAndResolvesPinnedPaths) {
  addBook("Zulu.txt");
  addBook("alpha.md");
  addBook("Middle.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  const std::array<std::string, 3> pinnedPaths = {"/Middle.txt", "/missing.epub", "/alpha.md"};
  std::vector<size_t> visible;
  std::vector<size_t> resolved;
  for (size_t steps = 0; steps < 20000; ++steps) {
    if (LIBRARY_CATALOG.loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, LibraryBookFormat::Text, visible,
                                           pinnedPaths, &resolved)) {
      break;
    }
    ASSERT_TRUE(LIBRARY_CATALOG.isOrderBuilding());
    LIBRARY_CATALOG.step();
  }
  ASSERT_EQ(visible.size(), 1U);
  LibraryBookRecord markdown;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(visible.front(), markdown));
  EXPECT_EQ(markdown.path, "/alpha.md");

  ASSERT_EQ(resolved.size(), 3U);
  EXPECT_NE(resolved[0], static_cast<size_t>(-1));
  EXPECT_EQ(resolved[1], static_cast<size_t>(-1));
  EXPECT_EQ(resolved[2], visible.front());
}

TEST_F(LibraryCatalogTest, CorruptOrderSidecarIsRegeneratedFromCommittedCatalog) {
  addBook("Zulu.txt");
  addBook("alpha.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> first;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, first));
  const fs::path order = root_ / ".crosspoint/library.order";
  ASSERT_TRUE(fs::exists(order));
  std::fstream file(order, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char last = 0;
  file.read(&last, 1);
  ASSERT_TRUE(file.good());
  last ^= 0x40;
  file.seekp(-1, std::ios::end);
  file.write(&last, 1);
  file.close();

  std::vector<size_t> rebuilt;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, rebuilt));
  EXPECT_EQ(rebuilt, first);
}

TEST_F(LibraryCatalogTest, CorruptOrderSidecarFallsBackWhenResolvingPinnedPaths) {
  addBook("Zulu.txt");
  addBook("alpha.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<size_t> ordered;
  ASSERT_TRUE(loadOrderedIndices(CrossPointSettings::LIBRARY_SORT_TITLE_ASC, ordered));
  const fs::path order = root_ / ".crosspoint/library.order";
  std::fstream file(order, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char last = 0;
  file.read(&last, 1);
  ASSERT_TRUE(file.good());
  last ^= 0x40;
  file.seekp(-1, std::ios::end);
  file.write(&last, 1);
  file.close();

  std::vector<size_t> resolved;
  ASSERT_TRUE(LIBRARY_CATALOG.findPathIndices({"/alpha.txt"}, resolved));
  ASSERT_EQ(resolved.size(), 1U);
  ASSERT_NE(resolved.front(), static_cast<size_t>(-1));
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(resolved.front(), record));
  EXPECT_EQ(record.path, "/alpha.txt");
}

TEST_F(LibraryCatalogTest, SinglePublishedPathUpdatesReadyCatalogWithoutFullRebuild) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  addBook("new.txt");
  LibraryCatalogStore::markDirtyPath("/new.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Updating);
  advanceToReady();

  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 2U);
  EXPECT_GT(LIBRARY_CATALOG.generation(), originalGeneration);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.dirty"));
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(0, 2, records));
  ASSERT_EQ(records.size(), 2U);
  EXPECT_TRUE(std::any_of(records.begin(), records.end(),
                          [](const LibraryBookRecord& record) { return record.path == "/new.txt"; }));
}

TEST_F(LibraryCatalogTest, SinglePathUpdateLocatesCopiesAndVerifiesOneRecordPerStep) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  addBook("four.txt");
  LibraryCatalogStore::markDirtyPath("/four.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Updating);

  // The committed catalog remains readable while the replacement is built.
  std::vector<LibraryBookRecord> committed;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(0, 3, committed));
  ASSERT_EQ(committed.size(), 3U);

  const fs::path temporary = root_ / ".crosspoint/library.idx.tmp";
  for (size_t record = 0; record < 3; ++record) {
    LIBRARY_CATALOG.step();
    EXPECT_FALSE(fs::exists(temporary));
  }
  LIBRARY_CATALOG.step();  // finish locate and write only the header
  ASSERT_TRUE(fs::exists(temporary));
  const uintmax_t headerSize = fs::file_size(temporary);

  std::array<uintmax_t, 3> sizes{};
  for (size_t record = 0; record < sizes.size(); ++record) {
    LIBRARY_CATALOG.step();
    sizes[record] = fs::file_size(temporary);
  }
  EXPECT_GT(sizes[0], headerSize);
  EXPECT_EQ(sizes[1] - sizes[0], sizes[2] - sizes[1]);
  advanceToReady();
  EXPECT_EQ(LIBRARY_CATALOG.count(), 4U);
}

TEST_F(LibraryCatalogTest, MultiplePendingPathsFallBackToFullRebuild) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  addBook("new-one.txt");
  LibraryCatalogStore::markDirtyPath("/new-one.txt");
  addBook("new-two.txt");
  LibraryCatalogStore::markDirtyPath("/new-two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  advanceToReady();

  EXPECT_EQ(LIBRARY_CATALOG.count(), 3U);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.dirty"));
}

TEST_F(LibraryCatalogTest, OversizedDirtyMarkerFallsBackToBoundedFullRebuild) {
  addBook("original.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  addBook("new.txt");
  {
    std::ofstream marker(root_ / ".crosspoint/library.dirty", std::ios::binary | std::ios::trunc);
    marker << std::string(64 * 1024, 'x');
    ASSERT_TRUE(marker.good());
  }

  ASSERT_TRUE(LIBRARY_CATALOG.open());
  advanceToReady();
  EXPECT_EQ(LIBRARY_CATALOG.count(), 2U);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.dirty"));
}

TEST_F(LibraryCatalogTest, SinglePublishedPathReplacesExistingRecordWithoutChangingCount) {
  addBook("book.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  {
    std::ofstream file(root_ / "book.txt", std::ios::binary | std::ios::trunc);
    file << "updated content";
  }
  LibraryCatalogStore::markDirtyPath("/book.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Updating);
  advanceToReady();

  EXPECT_EQ(LIBRARY_CATALOG.count(), 1U);
  EXPECT_GT(LIBRARY_CATALOG.generation(), originalGeneration);
  LibraryBookRecord record;
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecord(0, record));
  EXPECT_EQ(record.path, "/book.txt");
  EXPECT_EQ(record.sourceSize, 15U);
}

TEST_F(LibraryCatalogTest, SingleDeletedPathRemovesOnlyThatRecordWithoutRediscovery) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  ASSERT_TRUE(fs::remove(root_ / "two.txt"));
  LibraryCatalogStore::markDeletedPath("/two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Updating);
  advanceToReady();

  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 2U);
  EXPECT_GT(LIBRARY_CATALOG.generation(), originalGeneration);
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(0, 2, records));
  ASSERT_EQ(records.size(), 2U);
  std::vector<std::string> paths;
  paths.reserve(records.size());
  for (const auto& record : records) paths.push_back(record.path);
  std::sort(paths.begin(), paths.end());
  EXPECT_EQ(paths, (std::vector<std::string>{"/one.txt", "/three.txt"}));
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.dirty"));
}

TEST_F(LibraryCatalogTest, ExternalDeletionIsDetectedCooperativelyWithoutDirtyMarker) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  ASSERT_TRUE(fs::remove(root_ / "two.txt"));
  LIBRARY_CATALOG.invalidateSourceValidation();
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Enriching);
  advanceToReady();

  EXPECT_EQ(LIBRARY_CATALOG.count(), 2U);
  EXPECT_GT(LIBRARY_CATALOG.generation(), originalGeneration);
  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(0, 2, records));
  ASSERT_EQ(records.size(), 2U);
  std::vector<std::string> paths;
  paths.reserve(records.size());
  for (const auto& record : records) paths.push_back(record.path);
  std::sort(paths.begin(), paths.end());
  EXPECT_EQ(paths, (std::vector<std::string>{"/one.txt", "/three.txt"}));
}

TEST_F(LibraryCatalogTest, SourceValidationKeepsAnUnchangedCatalogAndGeneration) {
  addBook("one.txt");
  addBook("two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();
  const uint32_t originalGeneration = LIBRARY_CATALOG.generation();

  LIBRARY_CATALOG.invalidateSourceValidation();
  ASSERT_TRUE(LIBRARY_CATALOG.open());
  ASSERT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Enriching);
  LIBRARY_CATALOG.step();
  EXPECT_EQ(LIBRARY_CATALOG.phase(), LibraryCatalogStore::Phase::Enriching);
  LIBRARY_CATALOG.step();

  EXPECT_TRUE(LIBRARY_CATALOG.isReady());
  EXPECT_EQ(LIBRARY_CATALOG.count(), 2U);
  EXPECT_EQ(LIBRARY_CATALOG.generation(), originalGeneration);
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.work"));
}

TEST_F(LibraryCatalogTest, ResolvePinnedPathsAndLoadNonContiguousRecordsWithOneCatalog) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  const std::vector<std::string> paths = {"/three.txt", "/missing.txt", "/one.txt"};
  std::vector<size_t> indices;
  ASSERT_TRUE(LIBRARY_CATALOG.findPathIndices(paths, indices));
  ASSERT_EQ(indices.size(), paths.size());
  ASSERT_NE(indices[0], static_cast<size_t>(-1));
  EXPECT_EQ(indices[1], static_cast<size_t>(-1));
  ASSERT_NE(indices[2], static_cast<size_t>(-1));
  EXPECT_NE(indices[0], indices[2]);

  std::vector<LibraryBookRecord> records;
  const std::array<size_t, 2> selectedIndices = {indices[0], indices[2]};
  ASSERT_TRUE(LIBRARY_CATALOG.loadRecords(selectedIndices, records));
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].path, "/three.txt");
  EXPECT_EQ(records[1].path, "/one.txt");
}

TEST_F(LibraryCatalogTest, BatchLoadRejectsCorruptActiveCatalog) {
  addBook("one.txt");
  addBook("two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  const fs::path catalog = root_ / ".crosspoint/library.idx";
  std::fstream file(catalog, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char byte = 0;
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte ^= 0x01;
  file.seekp(-1, std::ios::end);
  file.write(&byte, 1);
  file.close();

  std::vector<LibraryBookRecord> records = {LibraryBookRecord{}};
  EXPECT_FALSE(LIBRARY_CATALOG.loadPage(0, 2, records));
  EXPECT_TRUE(records.empty());
}

TEST_F(LibraryCatalogTest, BatchLoadRejectsTruncatedActiveCatalog) {
  addBook("one.txt");
  addBook("two.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  const fs::path catalog = root_ / ".crosspoint/library.idx";
  const uintmax_t size = fs::file_size(catalog);
  ASSERT_GT(size, 0U);
  fs::resize_file(catalog, size - 1U);

  std::vector<LibraryBookRecord> records = {LibraryBookRecord{}};
  EXPECT_FALSE(LIBRARY_CATALOG.loadPage(0, 2, records));
  EXPECT_TRUE(records.empty());
}

TEST_F(LibraryCatalogTest, FindPathChecksPreferredThenScansCatalog) {
  addBook("one.txt");
  addBook("two.txt");
  addBook("three.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  std::vector<LibraryBookRecord> records;
  ASSERT_TRUE(LIBRARY_CATALOG.loadPage(0, 3, records));
  ASSERT_EQ(records.size(), 3U);

  size_t found = 99;
  EXPECT_EQ(LIBRARY_CATALOG.findPath(records[1].path, 1, found), LibraryCatalogStore::FindPathResult::Found);
  EXPECT_EQ(found, 1U);
  EXPECT_EQ(LIBRARY_CATALOG.findPath(records[2].path, 0, found), LibraryCatalogStore::FindPathResult::Found);
  EXPECT_EQ(found, 2U);
  EXPECT_EQ(LIBRARY_CATALOG.findPath("/missing.txt", 0, found), LibraryCatalogStore::FindPathResult::NotFound);
  EXPECT_EQ(found, 2U);
}

TEST_F(LibraryCatalogTest, FindPathReportsCorruptCatalogAsIoError) {
  addBook("one.txt");
  ASSERT_TRUE(LIBRARY_CATALOG.startRefresh());
  advanceToReady();

  const fs::path catalog = root_ / ".crosspoint/library.idx";
  std::fstream file(catalog, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char byte = 0;
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte ^= 0x01;
  file.seekp(-1, std::ios::end);
  file.write(&byte, 1);
  file.close();

  size_t found = 99;
  EXPECT_EQ(LIBRARY_CATALOG.findPath("/one.txt", 0, found), LibraryCatalogStore::FindPathResult::IoError);
  EXPECT_EQ(found, 99U);
}

TEST_F(LibraryCatalogTest, CancelBeforeFinalizeHandlesAreOpenedIsSafe) {
  LIBRARY_CATALOG.cancel();
  EXPECT_FALSE(fs::exists(root_ / ".crosspoint/library.idx.tmp"));
}
}  // namespace

bool isBookFileTransactionArtifact(const char*) { return false; }
