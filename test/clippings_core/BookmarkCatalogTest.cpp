#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "activities/reader/BookmarkCatalog.h"
#include "util/BookmarkUtil.h"

namespace {

class BookmarkCatalogTest : public ::testing::Test {
 protected:
  struct Document {
    BookmarkCatalog::DocumentLoadResult result = BookmarkCatalog::DocumentLoadResult::Loaded;
    std::vector<BookmarkEntry> bookmarks;
    BookmarkBookMetadata metadata;
  };

  void SetUp() override {
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    ASSERT_TRUE(Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str()));
  }

  std::string addDocument(const std::string& bookPath, const std::string& bookType,
                          const BookmarkEntry::PositionKind positionKind, const size_t bookmarkCount = 1,
                          const bool sourceExists = true) {
    const std::string canonicalPath = BookmarkUtil::getBookmarkPath(bookPath);
    addDirectoryEntry(canonicalPath);

    Document document;
    document.metadata = {bookPath, bookPath, "Author", bookType};
    document.bookmarks.resize(bookmarkCount);
    for (BookmarkEntry& bookmark : document.bookmarks) bookmark.positionKind = positionKind;
    documents_.emplace(canonicalPath, std::move(document));
    if (sourceExists) Storage.setFile(bookPath, {'b'});
    return canonicalPath;
  }

  void addDirectoryEntry(const std::string& canonicalPath, const std::string& suffix = {}) {
    const std::string directory = BookmarkUtil::getBookmarksDir();
    const std::string leaf = canonicalPath.substr(directory.size());
    // The host stub preserves a trailing slash literally, so the enumerated
    // child needs the extra separator that SdFat normalizes on-device.
    Storage.setFile(directory + "/" + leaf + suffix, {'j'});
  }

  BookmarkCatalog::Loader loader() {
    return [this](const std::string& path, std::vector<BookmarkEntry>& bookmarks, BookmarkBookMetadata& metadata) {
      ++loadCounts_[path];
      const auto document = documents_.find(path);
      if (document == documents_.end()) return BookmarkCatalog::DocumentLoadResult::Missing;
      bookmarks = document->second.bookmarks;
      metadata = document->second.metadata;
      return document->second.result;
    };
  }

  std::map<std::string, Document> documents_;
  std::map<std::string, size_t> loadCounts_;
};

TEST_F(BookmarkCatalogTest, LoadsValidMetadataAndPositionKinds) {
  addDocument("/Books/Novel.epub", "epub", BookmarkEntry::PositionKind::Epub, 2);
  addDocument("/Books/Notes.txt", "txt", BookmarkEntry::PositionKind::Text);
  addDocument("/Books/Pages.xtch", "xtc", BookmarkEntry::PositionKind::FixedLayout, 3);

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  ASSERT_EQ(catalog.entries.size(), 3U);
  EXPECT_EQ(catalog.skippedBooks, 0U);

  const auto novel = std::find_if(catalog.entries.begin(), catalog.entries.end(),
                                  [](const auto& entry) { return entry.book.path == "/Books/Novel.epub"; });
  ASSERT_NE(novel, catalog.entries.end());
  EXPECT_EQ(novel->bookmarkCount, 2U);
  EXPECT_TRUE(novel->bookExists);
}

TEST_F(BookmarkCatalogTest, SkipsHashMismatchInvalidDocumentAndMismatchedPositionKind) {
  const std::string hashOwner = "/Books/Hash owner.epub";
  const std::string hashPath = BookmarkUtil::getBookmarkPath(hashOwner);
  addDirectoryEntry(hashPath);
  Document hashMismatch;
  hashMismatch.metadata = {"/Books/Different.epub", "Different", "Author", "epub"};
  hashMismatch.bookmarks.resize(1);
  documents_.emplace(hashPath, std::move(hashMismatch));

  const std::string invalidPath = addDocument("/Books/Invalid.epub", "epub", BookmarkEntry::PositionKind::Epub);
  documents_.at(invalidPath).result = BookmarkCatalog::DocumentLoadResult::Invalid;

  const std::string kindPath = addDocument("/Books/Wrong kind.epub", "epub", BookmarkEntry::PositionKind::FixedLayout);
  Storage.setFile("/Books/Different.epub", {'b'});

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  EXPECT_TRUE(catalog.entries.empty());
  EXPECT_EQ(catalog.skippedBooks, 3U);
  EXPECT_EQ(loadCounts_[kindPath], 1U);
}

TEST_F(BookmarkCatalogTest, KeepsValidBookmarkForMissingBook) {
  addDocument("/Books/Missing.txt", "txt", BookmarkEntry::PositionKind::Text, 1, false);

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  ASSERT_EQ(catalog.entries.size(), 1U);
  EXPECT_FALSE(catalog.entries.front().bookExists);
}

TEST_F(BookmarkCatalogTest, DeduplicatesCanonicalBackupAndTemporaryNames) {
  const std::string canonical = addDocument("/Books/Deduplicated.epub", "epub", BookmarkEntry::PositionKind::Epub);
  addDirectoryEntry(canonical, ".bak");
  addDirectoryEntry(canonical, ".tmp");

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  ASSERT_EQ(catalog.entries.size(), 1U);
  EXPECT_EQ(loadCounts_[canonical], 1U);
}

TEST_F(BookmarkCatalogTest, CapsCatalogAtThirtyTwoBooks) {
  for (size_t index = 0; index < BookmarkCatalog::MAX_BOOKS + 1; ++index) {
    addDocument("/Books/Book-" + std::to_string(index) + ".epub", "epub", BookmarkEntry::PositionKind::Epub);
  }

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  EXPECT_EQ(catalog.entries.size(), BookmarkCatalog::MAX_BOOKS);
  EXPECT_TRUE(catalog.directoryTruncated);
  EXPECT_EQ(loadCounts_.size(), BookmarkCatalog::MAX_BOOKS);
}

TEST_F(BookmarkCatalogTest, InvalidHashPrefixesDoNotConsumeVisibleBookSlots) {
  std::vector<std::string> paths;
  for (size_t index = 0; index < 40; ++index) {
    paths.push_back(
        addDocument("/Books/Candidate-" + std::to_string(index) + ".epub", "epub", BookmarkEntry::PositionKind::Epub));
  }
  std::sort(paths.begin(), paths.end());
  for (size_t index = 0; index < 32; ++index) {
    documents_.at(paths[index]).result = BookmarkCatalog::DocumentLoadResult::Invalid;
  }

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  EXPECT_EQ(catalog.entries.size(), 8U);
  EXPECT_EQ(catalog.skippedBooks, 32U);
  EXPECT_EQ(loadCounts_.size(), 40U);
}

TEST_F(BookmarkCatalogTest, StopsScanningAfterOneHundredTwentyEightDirectoryEntries) {
  const std::string directory = BookmarkUtil::getBookmarksDir();
  for (size_t index = 0; index < BookmarkCatalog::MAX_DIRECTORY_ENTRIES + 1; ++index) {
    Storage.setFile(directory + "/noise-" + std::to_string(index), {'x'});
  }

  BookmarkCatalog::Catalog catalog;
  ASSERT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::Loaded);
  EXPECT_TRUE(catalog.entries.empty());
  EXPECT_TRUE(catalog.directoryTruncated);
  EXPECT_TRUE(loadCounts_.empty());
}

TEST_F(BookmarkCatalogTest, FailsClosedOnDirectoryIterationError) {
  addDocument("/Books/First.epub", "epub", BookmarkEntry::PositionKind::Epub);
  addDocument("/Books/Second.epub", "epub", BookmarkEntry::PositionKind::Epub);
  Storage.failDirectoryIterationAfter(1);

  BookmarkCatalog::Catalog catalog;
  EXPECT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::IoError);
  EXPECT_TRUE(catalog.entries.empty());
}

TEST_F(BookmarkCatalogTest, FailsClosedWhenDocumentLoaderReportsIoError) {
  const std::string canonical = addDocument("/Books/Unreadable.epub", "epub", BookmarkEntry::PositionKind::Epub);
  documents_.at(canonical).result = BookmarkCatalog::DocumentLoadResult::IoError;

  BookmarkCatalog::Catalog catalog;
  EXPECT_EQ(BookmarkCatalog::load(catalog, loader()), BookmarkCatalog::LoadResult::IoError);
  EXPECT_TRUE(catalog.entries.empty());
}

TEST(BookmarkUtilTest, SummaryTruncationKeepsVietnameseUtf8Whole) {
  std::string input;
  for (size_t index = 0; index < 25; ++index) input += "ế";

  const std::string summary = BookmarkUtil::sanitizeBookmarkSummary(input);

  std::string expected;
  for (size_t index = 0; index < 24; ++index) expected += "ế";
  EXPECT_EQ(summary, expected);
  EXPECT_EQ(summary.size(), 72U);
}

TEST(BookmarkUtilTest, MetadataMustMatchTheBookPathAndReaderKind) {
  const BookmarkBookMetadata legacy{};
  EXPECT_TRUE(BookmarkUtil::metadataMatchesBook(legacy, "/Books/Novel.epub", BookmarkEntry::PositionKind::Epub));
  EXPECT_TRUE(BookmarkUtil::metadataMatchesBook(legacy, "/Books/Notes.txt", BookmarkEntry::PositionKind::Text));

  const BookmarkBookMetadata epub{"/Books/Novel.epub", "Novel", "Author", "epub"};
  EXPECT_TRUE(BookmarkUtil::metadataMatchesBook(epub, "/Books/Novel.epub", BookmarkEntry::PositionKind::Epub));
  EXPECT_FALSE(BookmarkUtil::metadataMatchesBook(epub, "/Books/Other.epub", BookmarkEntry::PositionKind::Epub));
  EXPECT_FALSE(BookmarkUtil::metadataMatchesBook(epub, "/Books/Novel.epub", BookmarkEntry::PositionKind::Text));

  const BookmarkBookMetadata malformed{"", "Unexpected title", "", ""};
  EXPECT_FALSE(BookmarkUtil::metadataMatchesBook(malformed, "/Books/Novel.epub", BookmarkEntry::PositionKind::Epub));

  const BookmarkBookMetadata fixed{"/Books/Pages.xtch", "Pages", "", "xtc"};
  EXPECT_TRUE(BookmarkUtil::metadataMatchesBook(fixed, "/Books/Pages.xtch", BookmarkEntry::PositionKind::FixedLayout));
}

TEST(BookmarkUtilTest, FingerprintChangesWhenAReplacementBookmarkDiffers) {
  BookmarkEntry original;
  original.positionKind = BookmarkEntry::PositionKind::Text;
  original.byteOffset = 1234;
  original.summary = "Old source";
  BookmarkEntry same = original;
  BookmarkEntry replacement = original;
  replacement.summary = "New source";

  EXPECT_EQ(BookmarkUtil::fingerprint(original), BookmarkUtil::fingerprint(same));
  EXPECT_NE(BookmarkUtil::fingerprint(original), BookmarkUtil::fingerprint(replacement));
}

}  // namespace
