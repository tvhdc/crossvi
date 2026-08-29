#include "BookmarkUtil.h"

#include <HalStorage.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr size_t BOOKMARK_HASH_HEX_LENGTH = 16;

bool isHex(const char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

}  // namespace

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  uint64_t hash = FNV_OFFSET;
  for (const unsigned char byte : bookPath) {
    hash ^= byte;
    hash *= FNV_PRIME;
  }
  constexpr std::array<char, 16> HEX_DIGITS = {'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::array<char, 16> encoded{};
  for (size_t i = 0; i < encoded.size(); ++i) {
    const unsigned shift = static_cast<unsigned>((encoded.size() - 1 - i) * 4);
    encoded[i] = HEX_DIGITS[(hash >> shift) & 0x0FU];
  }
  return getBookmarksDir() + "book_" + std::string(encoded.data(), encoded.size()) + ".json";
}

std::string BookmarkUtil::getExistingCanonicalBookmarkPath(const std::string& bookPath) {
  const std::string canonical = getBookmarkPath(bookPath);
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    const std::string candidate = canonical + suffix;
    if (Storage.exists(candidate.c_str())) return candidate;
  }
  return {};
}

bool BookmarkUtil::canonicalFamilyExists(const std::string& bookPath) {
  return !getExistingCanonicalBookmarkPath(bookPath).empty();
}

std::string BookmarkUtil::getLegacyBookmarkPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getBookmarksDir() + bookName;
}

bool BookmarkUtil::isEmptyBookmarkFile(const std::string& path) {
  constexpr char EMPTY_BOOKMARKS[] = "{\"bookmarks\":[]}";
  HalFile file;
  std::array<char, sizeof(EMPTY_BOOKMARKS) - 1> bytes{};
  if (!Storage.openFileForRead("BKM", path, file) || file.fileSize64() != bytes.size() ||
      file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) {
    return false;
  }
  return std::memcmp(bytes.data(), EMPTY_BOOKMARKS, bytes.size()) == 0;
}

bool BookmarkUtil::writeEmptyBookmarkFile(const std::string& path) {
  constexpr char EMPTY_BOOKMARKS[] = "{\"bookmarks\":[]}";
  HalFile file;
  if (!Storage.openFileForWrite("BKM", path, file)) return false;
  const bool written = file.write(EMPTY_BOOKMARKS, sizeof(EMPTY_BOOKMARKS) - 1) == sizeof(EMPTY_BOOKMARKS) - 1;
  bool synced = false;
  if (written) {
    synced = file.sync();
  }
  const bool closed = file.close();
  if (!written || !synced || !closed) return false;
  return isEmptyBookmarkFile(path);
}

bool BookmarkUtil::writeEmptyCanonicalBookmark(const std::string& bookPath) {
  constexpr char ROOT[] = "/.crosspoint";
  const std::string directory = getBookmarksDir();
  if ((!Storage.exists(ROOT) && !Storage.mkdir(ROOT)) ||
      (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str()))) {
    return false;
  }
  return writeEmptyBookmarkFile(getBookmarkPath(bookPath));
}

bool BookmarkUtil::quarantineCanonicalForReplacement(const std::string& bookPath, const std::string& cachePath) {
  constexpr char ROOT[] = "/.crosspoint";
  // A replacement is rare; 256 preserved generations is generous while
  // bounding SD exists() calls if the archive namespace is pathological.
  constexpr unsigned MAX_ORPHAN_SLOTS = 256;
  if (bookPath.empty() || cachePath.empty() || cachePath == "/") return false;

  const std::string canonical = getBookmarkPath(bookPath);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  if (!Storage.exists(cachePath.c_str())) {
    // A retry may occur after the source cache was already quarantined. It is
    // complete only when the authoritative tombstone exists and no recovery
    // sibling can resurrect older bookmarks.
    return isEmptyBookmarkFile(canonical) && !Storage.exists((canonical + ".bak").c_str()) &&
           !Storage.exists((canonical + ".tmp").c_str());
  }
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    const std::string source = canonical + suffix;
    if (!Storage.exists(source.c_str()) || isEmptyBookmarkFile(source)) continue;

    std::string destination;
    for (unsigned slot = 0; slot < MAX_ORPHAN_SLOTS; ++slot) {
      std::string candidate = cachePath + "/.crossvi_replaced_bookmark.json";
      if (slot > 0) candidate += "." + std::to_string(slot + 1);
      if (!Storage.exists(candidate.c_str())) {
        destination = std::move(candidate);
        break;
      }
    }
    if (destination.empty() || !Storage.rename(source.c_str(), destination.c_str())) return false;
  }

  // Empty recovery siblings are redundant once a canonical tombstone exists.
  // Remove them so deleting the primary later cannot resurrect old state.
  constexpr std::array<const char*, 2> RECOVERY_SUFFIXES = {".bak", ".tmp"};
  const bool siblingsRemoved = std::all_of(RECOVERY_SUFFIXES.begin(), RECOVERY_SUFFIXES.end(), [&](const char* suffix) {
    const std::string sibling = canonical + suffix;
    return !Storage.exists(sibling.c_str()) || Storage.remove(sibling.c_str());
  });
  if (!siblingsRemoved) return false;
  if (Storage.exists(canonical.c_str())) {
    if (!isEmptyBookmarkFile(canonical)) return false;
    if (!Storage.exists(pending.c_str())) return true;
    // The verified canonical tombstone is authoritative. Any leftover pending
    // file, including a truncated one, is unpublished and safe to discard.
    return Storage.remove(pending.c_str());
  }

  const std::string directory = getBookmarksDir();
  if ((!Storage.exists(ROOT) && !Storage.mkdir(ROOT)) ||
      (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str()))) {
    return false;
  }
  if (Storage.exists(pending.c_str())) {
    // This name is exclusively our unpublished tombstone. A short write or
    // power loss may leave it truncated; the archived canonical bookmark and
    // source-identity barrier remain authoritative, so rebuilding the temp is
    // both safe and necessary for retry to make progress.
    if (!isEmptyBookmarkFile(pending) && !Storage.remove(pending.c_str())) return false;
    if (!Storage.exists(pending.c_str()) && !writeEmptyBookmarkFile(pending)) return false;
  } else if (!writeEmptyBookmarkFile(pending)) {
    return false;
  }
  return Storage.rename(pending.c_str(), canonical.c_str()) && isEmptyBookmarkFile(canonical);
}

bool BookmarkUtil::ensureLegacyBookmarkShadowed(const std::string& bookPath) {
  if (!Storage.exists(getLegacyBookmarkPath(bookPath).c_str())) return true;
  return writeEmptyCanonicalBookmark(bookPath);
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(),
                  [](const unsigned char a, const unsigned char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  if (summary.size() > 72) {
    summary.resize(static_cast<size_t>(utf8SafeTruncateBuffer(summary.c_str(), 72)));
  }
  return summary;
}

bool BookmarkUtil::isCanonicalBookmarkFileName(const std::string& fileName, std::string* canonicalName) {
  constexpr char PREFIX[] = "book_";
  constexpr char SUFFIX[] = ".json";
  constexpr char BACKUP_SUFFIX[] = ".bak";
  constexpr char TEMP_SUFFIX[] = ".tmp";
  const size_t canonicalLength = sizeof(PREFIX) - 1 + BOOKMARK_HASH_HEX_LENGTH + sizeof(SUFFIX) - 1;

  std::string_view name(fileName);
  if (name.ends_with(BACKUP_SUFFIX)) name.remove_suffix(sizeof(BACKUP_SUFFIX) - 1);
  if (name.ends_with(TEMP_SUFFIX)) name.remove_suffix(sizeof(TEMP_SUFFIX) - 1);
  if (name.size() != canonicalLength || !name.starts_with(PREFIX) || !name.ends_with(SUFFIX)) return false;

  const std::string_view hash = name.substr(sizeof(PREFIX) - 1, BOOKMARK_HASH_HEX_LENGTH);
  if (!std::all_of(hash.begin(), hash.end(), isHex)) return false;
  if (canonicalName) canonicalName->assign(name);
  return true;
}

bool BookmarkUtil::canonicalPathMatchesBook(const std::string& canonicalPath, const std::string& bookPath) {
  return canonicalPath == getBookmarkPath(bookPath);
}

bool BookmarkUtil::metadataMatchesBook(const BookmarkBookMetadata& metadata, const std::string& bookPath,
                                       const BookmarkEntry::PositionKind kind) {
  const bool legacy =
      metadata.path.empty() && metadata.title.empty() && metadata.author.empty() && metadata.bookType.empty();
  return legacy || (metadata.path == bookPath && metadata.bookType == positionKindName(kind));
}

uint64_t BookmarkUtil::fingerprint(const BookmarkEntry& bookmark) {
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  uint64_t hash = FNV_OFFSET;
  const auto append = [&](const void* data, const size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < length; ++index) {
      hash ^= bytes[index];
      hash *= FNV_PRIME;
    }
  };
  const auto appendString = [&](const std::string& value) {
    append(value.data(), value.size());
    const uint8_t separator = 0;
    append(&separator, sizeof(separator));
  };
  appendString(bookmark.xpath);
  appendString(bookmark.summary);
  // JSON serialization is allowed to change the last float bits. A fixed
  // point representation keeps the identity stable across save/load while
  // still distinguishing positions far below screen-level precision.
  const int32_t percentageFixed =
      static_cast<int32_t>(std::lround(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100000.0f));
  append(&percentageFixed, sizeof(percentageFixed));
  append(&bookmark.computedSpineIndex, sizeof(bookmark.computedSpineIndex));
  append(&bookmark.computedChapterPageCount, sizeof(bookmark.computedChapterPageCount));
  append(&bookmark.computedChapterProgress, sizeof(bookmark.computedChapterProgress));
  const uint8_t hasContentSourceOffset = bookmark.hasContentSourceOffset ? 1U : 0U;
  append(&hasContentSourceOffset, sizeof(hasContentSourceOffset));
  if (bookmark.hasContentSourceOffset) append(&bookmark.contentSourceOffset, sizeof(bookmark.contentSourceOffset));
  append(&bookmark.positionKind, sizeof(bookmark.positionKind));
  append(&bookmark.byteOffset, sizeof(bookmark.byteOffset));
  append(&bookmark.pageIndex, sizeof(bookmark.pageIndex));
  return hash;
}

const char* BookmarkUtil::positionKindName(const BookmarkEntry::PositionKind kind) {
  switch (kind) {
    case BookmarkEntry::PositionKind::Text:
      return "txt";
    case BookmarkEntry::PositionKind::FixedLayout:
      return "xtc";
    case BookmarkEntry::PositionKind::Epub:
    default:
      return "epub";
  }
}
