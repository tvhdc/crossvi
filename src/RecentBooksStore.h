#pragma once
#include <ArduinoJson.h>
#include <LazyStoreState.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 private:
  std::vector<RecentBook> recentBooks;
  std::vector<std::string> pinnedPaths;
  LazyStoreState loadState;

  static constexpr int MAX_RECENT_BOOKS = 10;
  static constexpr size_t MAX_PINNED_BOOKS = 12;
  static constexpr size_t MAX_PIN_PATH_BYTES = 512;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

 public:
  enum class PinResult : uint8_t { Pinned, Unpinned, LimitReached, InvalidPath, SaveFailed };
  enum class PruneStepResult : uint8_t { Pending, Complete, Removed, MediaUnavailable, SaveFailed };

  static const char* getFilePath() { return "/.crosspoint/recent.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();
  bool ensureLoaded();
  bool saveToFile() const;
  void markReadOnlyForRecovery();

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true only after the removal is durably persisted. A failed save restores the entry.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  PinResult togglePin(const std::string& path);
  bool isPinned(const std::string& path) const;
  const std::vector<std::string>& getPinnedPaths() const {
    const_cast<RecentBooksStore*>(this)->ensureLoaded();
    return pinnedPaths;
  }
  static constexpr size_t getMaxPinnedBooks() { return MAX_PINNED_BOOKS; }

  // Check at most one recent/pinned path, removing it durably when missing.
  // Cursors stay on a removed entry because the following item shifts into it.
  PruneStepResult pruneMissingStep(size_t& recentIndex, size_t& pinnedIndex, std::string* removedPath = nullptr);

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const {
    const_cast<RecentBooksStore*>(this)->ensureLoaded();
    return recentBooks;
  }

  // Get the count of recent books
  int getCount() const {
    const_cast<RecentBooksStore*>(this)->ensureLoaded();
    return static_cast<int>(recentBooks.size());
  }
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
