#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class RecentBooksStore;
class OpdsServerStore;
struct BookmarkEntry;
struct BookmarkBookMetadata;

namespace JsonSettingsIO {

constexpr size_t BOOKMARK_FILE_MAX_BYTES = 50U * 1024U;

enum class BookmarkLoadStatus : uint8_t { Loaded, Missing, Invalid, Oversize, IoError };
enum class BookmarkPathMoveStatus : uint8_t { Rewritten, Unchanged, Conflict, IoError };

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

// Bookmarks
bool saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path,
                   const BookmarkBookMetadata* metadata = nullptr);
bool loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json, BookmarkBookMetadata* metadata = nullptr);
BookmarkLoadStatus loadBookmarksFromFile(std::vector<BookmarkEntry>& bookmarks, const char* path,
                                         BookmarkBookMetadata* metadata = nullptr);
BookmarkPathMoveStatus classifyBookmarkMetadataForPathMove(const char* sourcePath, const std::string& sourceBookPath);
// Resolve the exact valid AtomicFile member used by a move while callers keep
// passing the canonical root. Invalid legacy payloads are preserved byte for
// byte from the first existing member; I/O errors fail closed.
bool resolveBookmarkDocumentForPathMove(const char* sourcePath, std::string& resolvedPath);
BookmarkPathMoveStatus stageBookmarkMetadataForPathMove(const char* sourcePath, const char* stagedPath,
                                                        const std::string& sourceBookPath,
                                                        const std::string& destinationBookPath);
bool validateBookmarkPathMove(const char* sourcePath, const char* movedPath, const std::string& sourceBookPath,
                              const std::string& destinationBookPath);

}  // namespace JsonSettingsIO
