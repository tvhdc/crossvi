#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "BookmarkEntry.h"

class BookmarkCatalog {
 public:
  static constexpr size_t MAX_BOOKS = 32;
  static constexpr size_t MAX_DIRECTORY_ENTRIES = 128;

  enum class DocumentLoadResult : uint8_t { Loaded, Missing, Invalid, IoError };
  enum class LoadResult : uint8_t { Loaded, DirectoryMissing, IoError };

  struct Entry {
    BookmarkBookMetadata book;
    std::string sourcePath;
    uint16_t bookmarkCount = 0;
    bool bookExists = false;
  };

  struct Catalog {
    std::vector<Entry> entries;
    uint16_t skippedBooks = 0;
    bool directoryTruncated = false;
    bool entryNameTruncated = false;
  };

  using Loader = std::function<DocumentLoadResult(
      const std::string& canonicalPath, std::vector<BookmarkEntry>& bookmarks, BookmarkBookMetadata& metadata)>;

  static LoadResult load(Catalog& catalog, const Loader& loader);
};
