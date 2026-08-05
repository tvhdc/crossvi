#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "BookmarkEntry.h"
#include "BookmarkUtil.h"

namespace {

constexpr char BOOKMARK_PATH[] = "/bookmarks.json";

std::vector<uint8_t> bytes(const std::string_view value) { return {value.begin(), value.end()}; }

constexpr std::string_view LEGACY_EPUB_JSON =
    R"({"bookmarks":[{"xpath":"OEBPS/chapter.xhtml","percentage":0.25,"summary":"Legacy","si":2,"pc":9,"pp":3}]})";

class BookmarkJsonIOTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(BookmarkJsonIOTest, LoadsLegacyJsonWithoutCatalogMetadata) {
  std::vector<BookmarkEntry> bookmarks;
  BookmarkBookMetadata metadata{"old", "old", "old", "old"};

  ASSERT_TRUE(JsonSettingsIO::loadBookmarks(bookmarks, std::string(LEGACY_EPUB_JSON).c_str(), &metadata));
  ASSERT_EQ(bookmarks.size(), 1U);
  EXPECT_EQ(bookmarks[0].positionKind, BookmarkEntry::PositionKind::Epub);
  EXPECT_EQ(bookmarks[0].xpath, "OEBPS/chapter.xhtml");
  EXPECT_FLOAT_EQ(bookmarks[0].percentage, 0.25F);
  EXPECT_EQ(bookmarks[0].computedSpineIndex, 2U);
  EXPECT_EQ(bookmarks[0].computedChapterPageCount, 9U);
  EXPECT_EQ(bookmarks[0].computedChapterProgress, 3U);
  EXPECT_FALSE(bookmarks[0].hasContentSourceOffset);
  EXPECT_EQ(bookmarks[0].contentSourceOffset, 0U);
  EXPECT_TRUE(metadata.path.empty());
  EXPECT_TRUE(metadata.title.empty());
  EXPECT_TRUE(metadata.author.empty());
  EXPECT_TRUE(metadata.bookType.empty());
}

TEST_F(BookmarkJsonIOTest, RoundTripsEpubContentSourceOffset) {
  BookmarkEntry bookmark;
  bookmark.xpath = "OEBPS/chapter.xhtml";
  bookmark.summary = "Stable content anchor";
  bookmark.percentage = 0.25F;
  bookmark.computedSpineIndex = 2;
  bookmark.computedChapterPageCount = 9;
  bookmark.computedChapterProgress = 3;
  bookmark.hasContentSourceOffset = true;
  bookmark.contentSourceOffset = 123456;
  const uint64_t savedFingerprint = BookmarkUtil::fingerprint(bookmark);

  ASSERT_TRUE(JsonSettingsIO::saveBookmarks({bookmark}, BOOKMARK_PATH));

  std::vector<BookmarkEntry> loaded;
  ASSERT_EQ(JsonSettingsIO::loadBookmarksFromFile(loaded, BOOKMARK_PATH), JsonSettingsIO::BookmarkLoadStatus::Loaded);
  ASSERT_EQ(loaded.size(), 1U);
  EXPECT_EQ(loaded[0].positionKind, BookmarkEntry::PositionKind::Epub);
  EXPECT_TRUE(loaded[0].hasContentSourceOffset);
  EXPECT_EQ(loaded[0].contentSourceOffset, bookmark.contentSourceOffset);
  EXPECT_EQ(BookmarkUtil::fingerprint(loaded[0]), savedFingerprint);
}

TEST_F(BookmarkJsonIOTest, RejectsMalformedOrNonEpubContentSourceOffset) {
  constexpr std::string_view cases[] = {
      R"({"bookmarks":[{"xpath":"chapter.xhtml","percentage":0.25,"summary":"Bad","sourceOffset":"12"}]})",
      R"({"bookmarks":[{"xpath":"text:12","percentage":0.25,"summary":"Bad","positionKind":"text","byteOffset":12,"sourceOffset":12}]})",
      R"({"bookmarks":[{"xpath":"fixed:2","percentage":0.25,"summary":"Bad","positionKind":"fixed","pageIndex":2,"sourceOffset":12}]})",
  };

  for (const auto json : cases) {
    std::vector<BookmarkEntry> bookmarks;
    EXPECT_FALSE(JsonSettingsIO::loadBookmarks(bookmarks, std::string(json).c_str())) << json;
  }
}

TEST_F(BookmarkJsonIOTest, RoundTripsFixedLayoutBookmarkAndCatalogMetadata) {
  BookmarkEntry bookmark;
  bookmark.xpath = "fixed:42";
  bookmark.summary = "Trang 43";
  bookmark.percentage = 0.5F;
  bookmark.positionKind = BookmarkEntry::PositionKind::FixedLayout;
  bookmark.pageIndex = 42;
  const BookmarkBookMetadata savedMetadata{"/Books/sample.xtch", "Sample", "Author", "xtc"};
  const uint64_t savedFingerprint = BookmarkUtil::fingerprint(bookmark);

  ASSERT_TRUE(JsonSettingsIO::saveBookmarks({bookmark}, BOOKMARK_PATH, &savedMetadata));

  std::vector<BookmarkEntry> loaded;
  BookmarkBookMetadata loadedMetadata;
  ASSERT_EQ(JsonSettingsIO::loadBookmarksFromFile(loaded, BOOKMARK_PATH, &loadedMetadata),
            JsonSettingsIO::BookmarkLoadStatus::Loaded);
  ASSERT_EQ(loaded.size(), 1U);
  EXPECT_EQ(loaded[0].positionKind, BookmarkEntry::PositionKind::FixedLayout);
  EXPECT_EQ(loaded[0].pageIndex, 42U);
  EXPECT_EQ(loaded[0].xpath, bookmark.xpath);
  EXPECT_EQ(loaded[0].summary, bookmark.summary);
  EXPECT_FLOAT_EQ(loaded[0].percentage, bookmark.percentage);
  EXPECT_EQ(BookmarkUtil::fingerprint(loaded[0]), savedFingerprint);
  EXPECT_EQ(loadedMetadata.path, savedMetadata.path);
  EXPECT_EQ(loadedMetadata.title, savedMetadata.title);
  EXPECT_EQ(loadedMetadata.author, savedMetadata.author);
  EXPECT_EQ(loadedMetadata.bookType, savedMetadata.bookType);
}

TEST_F(BookmarkJsonIOTest, RejectsPresentMalformedCatalogMetadata) {
  constexpr std::string_view cases[] = {
      R"({"book":"invalid","bookmarks":[]})",
      R"({"book":{"path":"/book.epub","title":"Book","author":"Author"},"bookmarks":[]})",
      R"({"book":{"path":"/book.epub","title":"Book","author":"Author","type":"EPUB"},"bookmarks":[]})",
  };

  for (const auto json : cases) {
    std::vector<BookmarkEntry> bookmarks;
    BookmarkBookMetadata metadata;
    EXPECT_FALSE(JsonSettingsIO::loadBookmarks(bookmarks, std::string(json).c_str(), &metadata)) << json;
  }
}

TEST_F(BookmarkJsonIOTest, RecoversValidBackupWhenPrimaryIsMissing) {
  Storage.setFile(std::string(BOOKMARK_PATH) + ".bak", bytes(LEGACY_EPUB_JSON));

  std::vector<BookmarkEntry> bookmarks;
  ASSERT_EQ(JsonSettingsIO::loadBookmarksFromFile(bookmarks, BOOKMARK_PATH),
            JsonSettingsIO::BookmarkLoadStatus::Loaded);
  ASSERT_EQ(bookmarks.size(), 1U);
  EXPECT_EQ(bookmarks[0].summary, "Legacy");
}

TEST_F(BookmarkJsonIOTest, MoveResolutionUsesValidBackupInsteadOfMalformedPrimary) {
  Storage.setFile(BOOKMARK_PATH, bytes("{not-json"));
  Storage.setFile(std::string(BOOKMARK_PATH) + ".bak", bytes(LEGACY_EPUB_JSON));

  std::string resolved;
  ASSERT_TRUE(JsonSettingsIO::resolveBookmarkDocumentForPathMove(BOOKMARK_PATH, resolved));
  EXPECT_EQ(resolved, std::string(BOOKMARK_PATH) + ".bak");
}

TEST_F(BookmarkJsonIOTest, RecoversValidTempWhenPrimaryAndBackupAreMissing) {
  Storage.setFile(std::string(BOOKMARK_PATH) + ".tmp", bytes(LEGACY_EPUB_JSON));

  std::vector<BookmarkEntry> bookmarks;
  ASSERT_EQ(JsonSettingsIO::loadBookmarksFromFile(bookmarks, BOOKMARK_PATH),
            JsonSettingsIO::BookmarkLoadStatus::Loaded);
  ASSERT_EQ(bookmarks.size(), 1U);
  EXPECT_EQ(bookmarks[0].summary, "Legacy");
}

}  // namespace
