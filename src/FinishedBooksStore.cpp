#include "FinishedBooksStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace {
constexpr size_t MAX_PATH_BYTES = 511;
constexpr size_t MAX_METADATA_BYTES = 255;
}  // namespace

void FinishedBooksStore::toJson(JsonDocument& doc) const {
  JsonArray array = doc["books"].to<JsonArray>();
  for (const FinishedBook& book : books_) {
    JsonObject object = array.add<JsonObject>();
    object["path"] = book.path;
    object["title"] = book.title;
    object["author"] = book.author;
    object["day"] = book.finishedDay;
  }
}

bool FinishedBooksStore::fromJson(const JsonVariantConst doc) {
  books_.clear();
  const JsonArrayConst array = doc["books"].as<JsonArrayConst>();
  books_.reserve(std::min(array.size(), MAX_BOOKS));
  for (const JsonObjectConst object : array) {
    if (books_.size() >= MAX_BOOKS) break;
    const char* path = object["path"] | "";
    const char* title = object["title"] | "";
    const char* author = object["author"] | "";
    if (path[0] == '\0' || std::char_traits<char>::length(path) > MAX_PATH_BYTES ||
        std::char_traits<char>::length(title) > MAX_METADATA_BYTES ||
        std::char_traits<char>::length(author) > MAX_METADATA_BYTES) {
      continue;
    }
    if (std::any_of(books_.begin(), books_.end(), [path](const FinishedBook& item) { return item.path == path; })) {
      continue;
    }
    books_.push_back({path, title, author, object["day"] | 0u});
  }
  return true;
}

bool FinishedBooksStore::ensureLoaded() {
  if (loadState_.usable()) return true;
  if (loadState_.failed() || !loadState_.beginLoad()) return false;
  const bool loaded = PersistableStore<FinishedBooksStore>::loadFromFile();
  const bool usable = loaded || isPersistenceWritable();
  if (!loaded && usable) books_.clear();
  loadState_.finish(usable);
  return usable;
}

bool FinishedBooksStore::saveToFile() const {
  if (!const_cast<FinishedBooksStore*>(this)->ensureLoaded()) return false;
  return PersistableStore<FinishedBooksStore>::saveToFile();
}

void FinishedBooksStore::markReadOnlyForRecovery() {
  books_.clear();
  loadState_.markLoaded();
  PersistableStore<FinishedBooksStore>::markReadOnlyForRecovery();
}

bool FinishedBooksStore::markCompleted(const std::string& path, const std::string& title, const std::string& author,
                                       const uint32_t finishedDay) {
  if (!ensureLoaded() || path.empty() || path.size() > MAX_PATH_BYTES || title.size() > MAX_METADATA_BYTES ||
      author.size() > MAX_METADATA_BYTES) {
    return false;
  }
  const std::vector<FinishedBook> previous = books_;
  books_.erase(
      std::remove_if(books_.begin(), books_.end(), [&path](const FinishedBook& book) { return book.path == path; }),
      books_.end());
  books_.insert(books_.begin(), {path, title, author, finishedDay});
  if (books_.size() > MAX_BOOKS) books_.resize(MAX_BOOKS);
  if (PersistableStore<FinishedBooksStore>::saveToFile()) return true;
  books_ = previous;
  LOG_ERR("FBS", "Failed to persist completed book: %s", path.c_str());
  return false;
}

bool FinishedBooksStore::removeByPath(const std::string& path) {
  if (!ensureLoaded()) return false;
  const auto item =
      std::find_if(books_.begin(), books_.end(), [&path](const FinishedBook& book) { return book.path == path; });
  if (item == books_.end()) return true;
  const size_t index = static_cast<size_t>(std::distance(books_.begin(), item));
  FinishedBook removed = std::move(*item);
  books_.erase(item);
  if (PersistableStore<FinishedBooksStore>::saveToFile()) return true;
  books_.insert(books_.begin() + index, std::move(removed));
  return false;
}

bool FinishedBooksStore::updatePath(const std::string& oldPath, const std::string& newPath) {
  if (!ensureLoaded() || newPath.empty() || newPath.size() > MAX_PATH_BYTES) return false;
  const auto item =
      std::find_if(books_.begin(), books_.end(), [&oldPath](const FinishedBook& book) { return book.path == oldPath; });
  if (item == books_.end()) return true;
  const std::string previous = item->path;
  item->path = newPath;
  if (PersistableStore<FinishedBooksStore>::saveToFile()) return true;
  item->path = previous;
  return false;
}

bool FinishedBooksStore::pruneMissing() {
  if (!ensureLoaded()) return false;
  const std::vector<FinishedBook> previous = books_;
  books_.erase(std::remove_if(books_.begin(), books_.end(),
                              [](const FinishedBook& book) { return !Storage.exists(book.path.c_str()); }),
               books_.end());
  if (books_.size() == previous.size()) return true;
  if (PersistableStore<FinishedBooksStore>::saveToFile()) return true;
  books_ = previous;
  return false;
}
