#pragma once

#include <string>
#include <utility>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string coverItemHref;
  };
};

class Epub {
 public:
  Epub(const std::string&, const std::string&) {}
  bool readCoreMetadata(BookMetadataCache::BookMetadata& output) const {
    if (!metadataAvailable) return false;
    output = metadata;
    return true;
  }
  std::string getThumbBmpPath() const { return thumbnailPath; }

  static void setMetadata(BookMetadataCache::BookMetadata value, std::string thumbnail) {
    metadata = std::move(value);
    thumbnailPath = std::move(thumbnail);
    metadataAvailable = true;
  }
  static void resetMetadata() {
    metadata = {};
    thumbnailPath.clear();
    metadataAvailable = false;
  }

 private:
  inline static BookMetadataCache::BookMetadata metadata{};
  inline static std::string thumbnailPath;
  inline static bool metadataAvailable = false;
};
