#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
  JsonArray pinned = doc["pinned"].to<JsonArray>();
  for (const auto& path : pinnedPaths) pinned.add(path);
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    const char* storedPath = obj["path"] | "";
    if (storedPath[0] == '\0') continue;
    RecentBook book;
    book.path = storedPath;
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    recentBooks.push_back(book);
  }

  pinnedPaths.clear();
  JsonArrayConst pinned = doc["pinned"].as<JsonArrayConst>();
  pinnedPaths.reserve(std::min(pinned.size(), MAX_PINNED_BOOKS));
  for (const JsonVariantConst value : pinned) {
    if (pinnedPaths.size() >= MAX_PINNED_BOOKS) break;
    const std::string path = value.as<const char*>() ? value.as<const char*>() : "";
    if (path.empty() || path.size() > MAX_PIN_PATH_BYTES ||
        !(FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
          FsHelpers::hasMarkdownExtension(path)) ||
        std::find(pinnedPaths.begin(), pinnedPaths.end(), path) != pinnedPaths.end()) {
      continue;
    }
    pinnedPaths.push_back(path);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

bool RecentBooksStore::loadFromFile() {
  if (loadState.usable()) return true;
  if (loadState.failed() || !loadState.beginLoad()) return false;
  const bool loaded = PersistableStore<RecentBooksStore>::loadFromFile();
  const bool usable = loaded || isPersistenceWritable();  // Missing file is a valid empty store.
  if (!loaded && usable) {
    recentBooks.clear();
    pinnedPaths.clear();
  }
  loadState.finish(usable);
  return usable;
}

bool RecentBooksStore::ensureLoaded() { return loadFromFile(); }

bool RecentBooksStore::saveToFile() const {
  if (!const_cast<RecentBooksStore*>(this)->ensureLoaded()) return false;
  return PersistableStore<RecentBooksStore>::saveToFile();
}

void RecentBooksStore::markReadOnlyForRecovery() {
  recentBooks.clear();
  pinnedPaths.clear();
  loadState.markLoaded();
  PersistableStore<RecentBooksStore>::markReadOnlyForRecovery();
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  if (!ensureLoaded()) return;
  if (!recentBooks.empty()) {
    const RecentBook& current = recentBooks.front();
    if (current.path == path && current.title == title && current.author == author &&
        current.coverBmpPath == coverBmpPath) {
      return;
    }
  }

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  if (!ensureLoaded()) return;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook previous = *it;
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    if (!saveToFile()) {
      *it = std::move(previous);
      LOG_ERR("RBS", "Failed to persist metadata update for recent book: %s", path.c_str());
    }
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  if (!ensureLoaded()) return false;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  const size_t index = static_cast<size_t>(std::distance(recentBooks.begin(), it));
  RecentBook removed = std::move(*it);
  recentBooks.erase(it);
  if (!saveToFile()) {
    recentBooks.insert(recentBooks.begin() + index, std::move(removed));
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
    return false;
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  if (!ensureLoaded()) return;
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  bool changed = false;
  if (it != recentBooks.end()) {
    it->path = newPath;
    if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
      it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
    }
    changed = true;
  }

  auto pin = std::find(pinnedPaths.begin(), pinnedPaths.end(), oldPath);
  if (pin != pinnedPaths.end() && newPath.size() <= MAX_PIN_PATH_BYTES) {
    const auto duplicate = std::find(pinnedPaths.begin(), pinnedPaths.end(), newPath);
    if (duplicate != pinnedPaths.end() && duplicate != pin) {
      pinnedPaths.erase(pin);
    } else {
      *pin = newPath;
    }
    changed = true;
  }
  if (changed) saveToFile();
}

RecentBooksStore::PinResult RecentBooksStore::togglePin(const std::string& path) {
  if (!ensureLoaded()) return PinResult::SaveFailed;
  auto existing = std::find(pinnedPaths.begin(), pinnedPaths.end(), path);
  if (existing != pinnedPaths.end()) {
    const size_t index = static_cast<size_t>(std::distance(pinnedPaths.begin(), existing));
    pinnedPaths.erase(existing);
    if (!saveToFile()) {
      pinnedPaths.insert(pinnedPaths.begin() + index, path);
      return PinResult::SaveFailed;
    }
    return PinResult::Unpinned;
  }

  if (path.empty() || path.size() > MAX_PIN_PATH_BYTES ||
      !(FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
        FsHelpers::hasMarkdownExtension(path))) {
    return PinResult::InvalidPath;
  }
  if (pinnedPaths.size() >= MAX_PINNED_BOOKS) return PinResult::LimitReached;

  pinnedPaths.insert(pinnedPaths.begin(), path);
  if (!saveToFile()) {
    pinnedPaths.erase(pinnedPaths.begin());
    return PinResult::SaveFailed;
  }
  return PinResult::Pinned;
}

bool RecentBooksStore::isPinned(const std::string& path) const {
  if (!const_cast<RecentBooksStore*>(this)->ensureLoaded()) return false;
  return std::find(pinnedPaths.begin(), pinnedPaths.end(), path) != pinnedPaths.end();
}

RecentBooksStore::PruneStepResult RecentBooksStore::pruneMissingStep(size_t& recentIndex, size_t& pinnedIndex,
                                                                     std::string* removedPath) {
  if (!ensureLoaded()) return PruneStepResult::SaveFailed;

  if (recentIndex < recentBooks.size()) {
    const std::string path = recentBooks[recentIndex].path;
    if (Storage.exists(path.c_str())) {
      ++recentIndex;
      return PruneStepResult::Pending;
    }
    if (!Storage.probeMedia()) return PruneStepResult::MediaUnavailable;

    RecentBook removedBook = std::move(recentBooks[recentIndex]);
    recentBooks.erase(recentBooks.begin() + recentIndex);
    const auto pin = std::find(pinnedPaths.begin(), pinnedPaths.end(), path);
    const size_t pinIndex = static_cast<size_t>(std::distance(pinnedPaths.begin(), pin));
    std::string removedPin;
    if (pin != pinnedPaths.end()) {
      removedPin = std::move(*pin);
      pinnedPaths.erase(pin);
      if (pinIndex < pinnedIndex) --pinnedIndex;
    }

    if (!saveToFile()) {
      recentBooks.insert(recentBooks.begin() + recentIndex, std::move(removedBook));
      if (!removedPin.empty()) {
        pinnedPaths.insert(pinnedPaths.begin() + pinIndex, std::move(removedPin));
        if (pinIndex < pinnedIndex) ++pinnedIndex;
      }
      return PruneStepResult::SaveFailed;
    }
    if (removedPath) *removedPath = path;
    return PruneStepResult::Removed;
  }

  if (pinnedIndex < pinnedPaths.size()) {
    const std::string path = pinnedPaths[pinnedIndex];
    if (Storage.exists(path.c_str())) {
      ++pinnedIndex;
      return PruneStepResult::Pending;
    }
    if (!Storage.probeMedia()) return PruneStepResult::MediaUnavailable;

    std::string removedPin = std::move(pinnedPaths[pinnedIndex]);
    pinnedPaths.erase(pinnedPaths.begin() + pinnedIndex);
    if (!saveToFile()) {
      pinnedPaths.insert(pinnedPaths.begin() + pinnedIndex, std::move(removedPin));
      return PruneStepResult::SaveFailed;
    }
    if (removedPath) *removedPath = path;
    return PruneStepResult::Removed;
  }

  return PruneStepResult::Complete;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}
