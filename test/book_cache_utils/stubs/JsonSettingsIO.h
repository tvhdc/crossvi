#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace JsonSettingsIO {

enum class BookmarkPathMoveStatus : uint8_t { Rewritten, Unchanged, Conflict, IoError };

namespace detail {

struct BookmarkDocument {
  std::string path;
  std::string title;
  std::string author;
  std::string type;
  std::string payload;
};

inline bool parseBookmarkDocument(const std::vector<unsigned char>& bytes, BookmarkDocument& document) {
  constexpr std::string_view prefix = "BKMETA\n";
  const std::string serialized(bytes.begin(), bytes.end());
  if (serialized.compare(0, prefix.size(), prefix) != 0) return false;

  size_t begin = prefix.size();
  std::string* fields[] = {&document.path, &document.title, &document.author, &document.type};
  for (std::string* field : fields) {
    const size_t end = serialized.find('\n', begin);
    if (end == std::string::npos) return false;
    *field = serialized.substr(begin, end - begin);
    begin = end + 1;
  }
  document.payload = serialized.substr(begin);
  return !document.path.empty() && !document.type.empty();
}

inline std::vector<unsigned char> serializeBookmarkDocument(const BookmarkDocument& document) {
  const std::string serialized = "BKMETA\n" + document.path + "\n" + document.title + "\n" + document.author + "\n" +
                                 document.type + "\n" + document.payload;
  return std::vector<unsigned char>(serialized.begin(), serialized.end());
}

}  // namespace detail

inline std::vector<unsigned char> bookmarkDocumentForTest(const std::string& path, const std::string& title,
                                                          const std::string& author, const std::string& type,
                                                          const std::string& payload = "bookmarks") {
  return detail::serializeBookmarkDocument({path, title, author, type, payload});
}

inline bool resolveBookmarkDocumentForPathMove(const char* sourcePath, std::string& resolvedPath) {
  resolvedPath.clear();
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    const std::string candidate = std::string(sourcePath) + suffix;
    if (Storage.exists(candidate.c_str())) {
      resolvedPath = candidate;
      return true;
    }
  }
  return false;
}

inline BookmarkPathMoveStatus classifyBookmarkMetadataForPathMove(const char* sourcePath,
                                                                  const std::string& sourceBookPath) {
  std::string resolvedPath;
  if (!resolveBookmarkDocumentForPathMove(sourcePath, resolvedPath)) return BookmarkPathMoveStatus::IoError;
  detail::BookmarkDocument document;
  if (!detail::parseBookmarkDocument(Storage.file(resolvedPath), document)) return BookmarkPathMoveStatus::Unchanged;
  return document.path == sourceBookPath ? BookmarkPathMoveStatus::Rewritten : BookmarkPathMoveStatus::Conflict;
}

inline BookmarkPathMoveStatus stageBookmarkMetadataForPathMove(const char* sourcePath, const char* stagedPath,
                                                               const std::string& sourceBookPath,
                                                               const std::string& destinationBookPath) {
  const BookmarkPathMoveStatus status = classifyBookmarkMetadataForPathMove(sourcePath, sourceBookPath);
  if (status != BookmarkPathMoveStatus::Rewritten) return status;
  std::string resolvedPath;
  if (!resolveBookmarkDocumentForPathMove(sourcePath, resolvedPath)) return BookmarkPathMoveStatus::IoError;
  detail::BookmarkDocument document;
  if (!detail::parseBookmarkDocument(Storage.file(resolvedPath), document)) return BookmarkPathMoveStatus::IoError;
  document.path = destinationBookPath;
  Storage.setFile(stagedPath, detail::serializeBookmarkDocument(document));
  return BookmarkPathMoveStatus::Rewritten;
}

inline bool validateBookmarkPathMove(const char* sourcePath, const char* movedPath, const std::string& sourceBookPath,
                                     const std::string& destinationBookPath) {
  std::string resolvedPath;
  if (!resolveBookmarkDocumentForPathMove(sourcePath, resolvedPath)) return false;
  detail::BookmarkDocument source;
  detail::BookmarkDocument moved;
  if (!detail::parseBookmarkDocument(Storage.file(resolvedPath), source) ||
      !detail::parseBookmarkDocument(Storage.file(movedPath), moved)) {
    return false;
  }
  return source.path == sourceBookPath && moved.path == destinationBookPath && source.title == moved.title &&
         source.author == moved.author && source.type == moved.type && source.payload == moved.payload;
}

}  // namespace JsonSettingsIO
