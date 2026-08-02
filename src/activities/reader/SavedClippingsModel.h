#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "BookmarkCatalog.h"
#include "clippings/ClippingStore.h"

namespace SavedClippingsModel {

enum class CatalogState : uint8_t {
  Ready,
  Empty,
  Incomplete,
  ReadError,
};

struct SavedBookEntry {
  std::string path;
  std::string title;
  std::string author;
  std::string bookType;
  int clippingIndex = -1;
  int bookmarkIndex = -1;
  uint16_t bookmarkCount = 0;
  uint16_t highlightCount = 0;
  bool bookExists = false;
};

struct CombinedCatalog {
  std::vector<SavedBookEntry> entries;
  bool incomplete = false;
  bool readError = false;
};

inline bool hasKnownTimestamp(const ClippingStore::CatalogEntry& entry) {
  // Legacy CrossInk timestamps are not trustworthy, and an unset/broken
  // device clock must not become a plausible-looking date in the UI.
  constexpr uint32_t MIN_TIMESTAMP = 1577836800U;  // 2020-01-01 UTC
  constexpr uint32_t MAX_TIMESTAMP = 4102444799U;  // 2099-12-31 UTC
  return entry.format == ClippingCodec::Format::Current && entry.newestTimestamp >= MIN_TIMESTAMP &&
         entry.newestTimestamp <= MAX_TIMESTAMP;
}

inline bool isComplete(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  return loadResult == ClippingStore::CatalogLoadResult::Loaded &&
         catalog.entries.size() <= ClippingStore::MAX_CATALOG_BOOKS && !catalog.directoryTruncated &&
         !catalog.entryNameTruncated && catalog.skippedBooks == 0;
}

inline CatalogState state(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  if (loadResult == ClippingStore::CatalogLoadResult::IoError) return CatalogState::ReadError;
  if (loadResult == ClippingStore::CatalogLoadResult::DirectoryMissing) return CatalogState::Empty;
  if (!isComplete(loadResult, catalog)) return CatalogState::Incomplete;
  return catalog.entries.empty() ? CatalogState::Empty : CatalogState::Ready;
}

inline bool canExport(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  return state(loadResult, catalog) == CatalogState::Ready;
}

inline void sortEntries(std::vector<ClippingStore::CatalogEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    const bool leftKnown = hasKnownTimestamp(left);
    const bool rightKnown = hasKnownTimestamp(right);
    if (leftKnown != rightKnown) return leftKnown;
    if (leftKnown && left.newestTimestamp != right.newestTimestamp) {
      return left.newestTimestamp > right.newestTimestamp;
    }

    // Untitled books remain deterministic but do not hide named books at the
    // top of the unknown-time group.
    if (left.book.title.empty() != right.book.title.empty()) return !left.book.title.empty();
    if (left.book.title != right.book.title) return left.book.title < right.book.title;
    if (left.book.author != right.book.author) return left.book.author < right.book.author;
    return left.book.path < right.book.path;
  });
}

inline CombinedCatalog combine(const ClippingStore::CatalogLoadResult clippingLoad,
                               const ClippingStore::Catalog& clippings, const BookmarkCatalog::LoadResult bookmarkLoad,
                               const BookmarkCatalog::Catalog& bookmarks) {
  CombinedCatalog combined;
  combined.readError =
      clippingLoad == ClippingStore::CatalogLoadResult::IoError || bookmarkLoad == BookmarkCatalog::LoadResult::IoError;
  combined.incomplete = clippings.directoryTruncated || clippings.entryNameTruncated || clippings.skippedBooks != 0 ||
                        bookmarks.directoryTruncated || bookmarks.entryNameTruncated || bookmarks.skippedBooks != 0;
  combined.entries.reserve(
      std::min<size_t>(ClippingStore::MAX_CATALOG_BOOKS, clippings.entries.size() + bookmarks.entries.size()));

  const auto getOrAdd = [&combined](const std::string& path) -> SavedBookEntry* {
    const auto found = std::find_if(combined.entries.begin(), combined.entries.end(),
                                    [&](const SavedBookEntry& entry) { return entry.path == path; });
    if (found != combined.entries.end()) return &*found;
    if (combined.entries.size() >= ClippingStore::MAX_CATALOG_BOOKS) {
      combined.incomplete = true;
      return nullptr;
    }
    combined.entries.push_back({});
    combined.entries.back().path = path;
    return &combined.entries.back();
  };

  for (size_t index = 0; index < clippings.entries.size(); ++index) {
    const auto& source = clippings.entries[index];
    SavedBookEntry* target = getOrAdd(source.book.path);
    if (!target) continue;
    target->title = source.book.title;
    target->author = source.book.author;
    target->bookType = source.book.bookType;
    target->clippingIndex = static_cast<int>(index);
    target->highlightCount = source.clippingCount;
    target->bookExists = source.bookExists;
  }
  for (size_t index = 0; index < bookmarks.entries.size(); ++index) {
    const auto& source = bookmarks.entries[index];
    SavedBookEntry* target = getOrAdd(source.book.path);
    if (!target) continue;
    if (!target->bookType.empty() && target->bookType != source.book.bookType) {
      combined.incomplete = true;
      continue;
    }
    if (target->title.empty()) target->title = source.book.title;
    if (target->author.empty()) target->author = source.book.author;
    if (target->bookType.empty()) target->bookType = source.book.bookType;
    target->bookmarkIndex = static_cast<int>(index);
    target->bookmarkCount = source.bookmarkCount;
    target->bookExists = target->bookExists || source.bookExists;
  }
  std::sort(combined.entries.begin(), combined.entries.end(),
            [](const SavedBookEntry& left, const SavedBookEntry& right) {
              if (left.title.empty() != right.title.empty()) return !left.title.empty();
              if (left.title != right.title) return left.title < right.title;
              return left.path < right.path;
            });
  return combined;
}

}  // namespace SavedClippingsModel
