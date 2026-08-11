#pragma once

#include <ArduinoJson.h>
#include <LazyStoreState.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FinishedBook {
  std::string path;
  std::string title;
  std::string author;
  uint32_t finishedDay = 0;
};

class FinishedBooksStore final : public PersistableStore<FinishedBooksStore> {
 public:
  static constexpr size_t MAX_BOOKS = 32;

  static const char* getFilePath() { return "/.crosspoint/finished_books.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool ensureLoaded();
  bool saveToFile() const;
  void markReadOnlyForRecovery();

  bool markCompleted(const std::string& path, const std::string& title, const std::string& author,
                     uint32_t finishedDay);
  bool removeByPath(const std::string& path);
  bool updatePath(const std::string& oldPath, const std::string& newPath);
  bool pruneMissing();

  const std::vector<FinishedBook>& books() const {
    const_cast<FinishedBooksStore*>(this)->ensureLoaded();
    return books_;
  }

 private:
  FinishedBooksStore() = default;
  friend class PersistableStore<FinishedBooksStore>;

  std::vector<FinishedBook> books_;
  LazyStoreState loadState_;
};

#define FINISHED_BOOKS FinishedBooksStore::getInstance()
