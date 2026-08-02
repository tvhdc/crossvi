#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "activities/reader/SavedClippingsModel.h"

namespace {

ClippingStore::CatalogEntry entry(std::string title, const uint32_t timestamp,
                                  const ClippingCodec::Format format = ClippingCodec::Format::Current) {
  ClippingStore::CatalogEntry value;
  value.book.title = std::move(title);
  value.book.path = "/books/" + value.book.title + ".epub";
  value.format = format;
  value.newestTimestamp = timestamp;
  value.clippingCount = 1;
  return value;
}

TEST(SavedClippingsModelTest, SortsKnownCurrentTimestampsNewestFirstThenUnknownBooksByTitle) {
  std::vector<ClippingStore::CatalogEntry> entries{
      entry("Zulu", 0),
      entry("Older", 1700000000U),
      entry("Legacy", 1900000000U, ClippingCodec::Format::CrossInkV2),
      entry("Newest", 1800000000U),
      entry("Future", 4200000000U),
      entry("Alpha", 0),
  };

  SavedClippingsModel::sortEntries(entries);

  ASSERT_EQ(entries.size(), 6U);
  EXPECT_EQ(entries[0].book.title, "Newest");
  EXPECT_EQ(entries[1].book.title, "Older");
  EXPECT_EQ(entries[2].book.title, "Alpha");
  EXPECT_EQ(entries[3].book.title, "Future");
  EXPECT_EQ(entries[4].book.title, "Legacy");
  EXPECT_EQ(entries[5].book.title, "Zulu");
}

TEST(SavedClippingsModelTest, KeepsUntitledUnknownBooksAfterNamedBooks) {
  std::vector<ClippingStore::CatalogEntry> entries{entry("", 0), entry("Named", 0)};

  SavedClippingsModel::sortEntries(entries);

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].book.title, "Named");
  EXPECT_TRUE(entries[1].book.title.empty());
}

TEST(SavedClippingsModelTest, DistinguishesEmptyIncompleteAndReadFailure) {
  ClippingStore::Catalog catalog;
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::DirectoryMissing, catalog),
            SavedClippingsModel::CatalogState::Empty);
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::IoError, catalog),
            SavedClippingsModel::CatalogState::ReadError);

  catalog.entryNameTruncated = true;
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Incomplete);
  EXPECT_FALSE(SavedClippingsModel::canExport(ClippingStore::CatalogLoadResult::Loaded, catalog));

  catalog = {};
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Empty);
  catalog.entries.push_back(entry("Book", 1700000000U));
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Ready);
  EXPECT_TRUE(SavedClippingsModel::canExport(ClippingStore::CatalogLoadResult::Loaded, catalog));
}

TEST(SavedClippingsModelTest, EveryBoundedOrSkippedCatalogIsIncomplete) {
  ClippingStore::Catalog catalog;
  catalog.entries.push_back(entry("Book", 1700000000U));

  catalog.directoryTruncated = true;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
  catalog.directoryTruncated = false;
  catalog.entryNameTruncated = true;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
  catalog.entryNameTruncated = false;
  catalog.skippedBooks = 1;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
}

TEST(SavedClippingsModelTest, CombinesBookmarkAndHighlightCountsByBookPath) {
  ClippingStore::Catalog clippings;
  auto clipping = entry("Book", 1700000000U);
  clipping.book.author = "Author";
  clipping.book.bookType = "epub";
  clipping.clippingCount = 3;
  clipping.bookExists = true;
  clippings.entries.push_back(clipping);

  BookmarkCatalog::Catalog bookmarks;
  BookmarkCatalog::Entry bookmark;
  bookmark.book.path = clipping.book.path;
  bookmark.book.title = "Book";
  bookmark.book.author = "Author";
  bookmark.book.bookType = "epub";
  bookmark.bookmarkCount = 2;
  bookmark.bookExists = true;
  bookmarks.entries.push_back(bookmark);

  const auto combined = SavedClippingsModel::combine(ClippingStore::CatalogLoadResult::Loaded, clippings,
                                                     BookmarkCatalog::LoadResult::Loaded, bookmarks);
  ASSERT_EQ(combined.entries.size(), 1U);
  EXPECT_EQ(combined.entries[0].bookmarkCount, 2U);
  EXPECT_EQ(combined.entries[0].highlightCount, 3U);
  EXPECT_GE(combined.entries[0].bookmarkIndex, 0);
  EXPECT_GE(combined.entries[0].clippingIndex, 0);
}

TEST(SavedClippingsModelTest, KeepsBookmarkOnlyMissingBookAndReportsBoundedInput) {
  ClippingStore::Catalog clippings;
  BookmarkCatalog::Catalog bookmarks;
  BookmarkCatalog::Entry bookmark;
  bookmark.book.path = "/missing.xtc";
  bookmark.book.title = "Missing";
  bookmark.book.bookType = "xtc";
  bookmark.bookmarkCount = 1;
  bookmarks.entries.push_back(bookmark);
  bookmarks.directoryTruncated = true;

  const auto combined = SavedClippingsModel::combine(ClippingStore::CatalogLoadResult::DirectoryMissing, clippings,
                                                     BookmarkCatalog::LoadResult::Loaded, bookmarks);
  ASSERT_EQ(combined.entries.size(), 1U);
  EXPECT_FALSE(combined.entries[0].bookExists);
  EXPECT_TRUE(combined.incomplete);
}

TEST(SavedClippingsModelTest, DoesNotMergeConflictingReaderKindsForTheSamePath) {
  ClippingStore::Catalog clippings;
  ClippingStore::CatalogEntry clipping;
  clipping.book.title = "Conflicted";
  clipping.book.path = "/Books/Conflicted.epub";
  clipping.book.bookType = "epub";
  clipping.clippingCount = 2;
  clipping.bookExists = true;
  clippings.entries.push_back(clipping);

  BookmarkCatalog::Catalog bookmarks;
  BookmarkCatalog::Entry bookmark;
  bookmark.book = {"/Books/Conflicted.epub", "Conflicted", "", "txt"};
  bookmark.bookmarkCount = 3;
  bookmark.bookExists = true;
  bookmarks.entries.push_back(bookmark);

  const auto combined = SavedClippingsModel::combine(ClippingStore::CatalogLoadResult::Loaded, clippings,
                                                     BookmarkCatalog::LoadResult::Loaded, bookmarks);

  ASSERT_EQ(combined.entries.size(), 1U);
  EXPECT_EQ(combined.entries[0].highlightCount, 2U);
  EXPECT_EQ(combined.entries[0].bookmarkCount, 0U);
  EXPECT_TRUE(combined.incomplete);
}

}  // namespace
