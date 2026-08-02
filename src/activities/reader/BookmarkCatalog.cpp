#include "BookmarkCatalog.h"

#include <HalStorage.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "util/BookmarkUtil.h"

namespace {

bool typeMatches(const BookmarkEntry& bookmark, const std::string& type) {
  if (type == "epub") return bookmark.positionKind == BookmarkEntry::PositionKind::Epub;
  if (type == "txt") return bookmark.positionKind == BookmarkEntry::PositionKind::Text;
  if (type == "xtc") {
    return bookmark.positionKind == BookmarkEntry::PositionKind::FixedLayout &&
           bookmark.pageIndex < std::numeric_limits<uint16_t>::max();
  }
  return false;
}

bool regularFileExists(const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  if (!file) return false;
  const bool regular = !file.isDirectory();
  return file.close() && regular;
}

}  // namespace

BookmarkCatalog::LoadResult BookmarkCatalog::load(Catalog& catalog, const Loader& loader) {
  catalog = {};
  const std::string directoryPath = BookmarkUtil::getBookmarksDir();
  if (!Storage.exists(directoryPath.c_str())) return LoadResult::DirectoryMissing;

  HalFile directory = Storage.open(directoryPath.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return LoadResult::IoError;
  }

  std::vector<std::string> canonicalNames;
  canonicalNames.reserve(MAX_DIRECTORY_ENTRIES);
  char name[64]{};
  size_t directoryEntries = 0;
  while (true) {
    HalFile file = directory.openNextFile();
    if (!file) {
      if (directory.getError() != 0) {
        directory.close();
        return LoadResult::IoError;
      }
      break;
    }
    if (directoryEntries == MAX_DIRECTORY_ENTRIES) {
      catalog.directoryTruncated = true;
      if (!file.close()) {
        directory.close();
        return LoadResult::IoError;
      }
      break;
    }
    ++directoryEntries;

    const size_t nameLength = file.getName(name, sizeof(name));
    if (nameLength == 0 || nameLength >= sizeof(name)) {
      const bool ioError = file.getError() != 0;
      const bool closed = file.close();
      if (ioError || !closed) {
        directory.close();
        return LoadResult::IoError;
      }
      catalog.entryNameTruncated = true;
      continue;
    }
    std::string canonicalName;
    const bool recognized = BookmarkUtil::isCanonicalBookmarkFileName(std::string(name, nameLength), &canonicalName);
    if (!file.close()) {
      directory.close();
      return LoadResult::IoError;
    }
    if (!recognized) continue;

    const auto position = std::lower_bound(canonicalNames.begin(), canonicalNames.end(), canonicalName);
    if (position != canonicalNames.end() && *position == canonicalName) continue;
    canonicalNames.insert(position, std::move(canonicalName));
  }
  if (!directory.close()) return LoadResult::IoError;

  catalog.entries.reserve(std::min(MAX_BOOKS, canonicalNames.size()));
  for (const std::string& nameValue : canonicalNames) {
    if (catalog.entries.size() == MAX_BOOKS) {
      // The scan remains bounded at 128 directory entries, but invalid files
      // encountered earlier must not consume one of the 32 visible book slots.
      catalog.directoryTruncated = true;
      break;
    }
    const std::string canonicalPath = directoryPath + nameValue;
    std::vector<BookmarkEntry> bookmarks;
    BookmarkBookMetadata metadata;
    const DocumentLoadResult loaded = loader(canonicalPath, bookmarks, metadata);
    if (loaded == DocumentLoadResult::IoError) return LoadResult::IoError;
    const bool valid =
        loaded == DocumentLoadResult::Loaded && !bookmarks.empty() && !metadata.path.empty() &&
        !metadata.bookType.empty() && BookmarkUtil::canonicalPathMatchesBook(canonicalPath, metadata.path) &&
        std::all_of(bookmarks.begin(), bookmarks.end(),
                    [&](const BookmarkEntry& bookmark) { return typeMatches(bookmark, metadata.bookType); });
    if (!valid) {
      if (catalog.skippedBooks != std::numeric_limits<uint16_t>::max()) ++catalog.skippedBooks;
      continue;
    }

    Entry entry;
    entry.book = std::move(metadata);
    entry.sourcePath = canonicalPath;
    entry.bookmarkCount = static_cast<uint16_t>(bookmarks.size());
    entry.bookExists = regularFileExists(entry.book.path);
    catalog.entries.push_back(std::move(entry));
  }
  std::sort(catalog.entries.begin(), catalog.entries.end(), [](const Entry& left, const Entry& right) {
    if (left.book.title != right.book.title) return left.book.title < right.book.title;
    return left.book.path < right.book.path;
  });
  return LoadResult::Loaded;
}
