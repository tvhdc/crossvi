#pragma once

#include <cstddef>
#include <cstdint>
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
  enum class CoreMetadataStepResult : uint8_t { InProgress, Loaded, Error };

  Epub(const std::string&, const std::string&) {}
  bool readCoreMetadata(BookMetadataCache::BookMetadata& output) const {
    if (!metadataAvailable) return false;
    output = metadata;
    return true;
  }
  bool beginCoreMetadataRead() {
    ++metadataBeginCalls_;
    readingMetadata_ = true;
    remainingSteps_ = configuredInProgressSteps_;
    return true;
  }
  CoreMetadataStepResult stepCoreMetadataRead(BookMetadataCache::BookMetadata& output) {
    if (!readingMetadata_) return CoreMetadataStepResult::Error;
    ++metadataStepCalls_;
    if (remainingSteps_ > 0) {
      --remainingSteps_;
      return CoreMetadataStepResult::InProgress;
    }
    readingMetadata_ = false;
    if (!metadataAvailable) return CoreMetadataStepResult::Error;
    output = metadata;
    return CoreMetadataStepResult::Loaded;
  }
  void cancelCoreMetadataRead() { readingMetadata_ = false; }
  std::string getThumbBmpPath() const { return thumbnailPath; }

  static void setMetadata(BookMetadataCache::BookMetadata value, std::string thumbnail,
                          const size_t inProgressSteps = 0) {
    metadata = std::move(value);
    thumbnailPath = std::move(thumbnail);
    configuredInProgressSteps_ = inProgressSteps;
    metadataAvailable = true;
  }
  static void resetMetadata() {
    metadata = {};
    thumbnailPath.clear();
    metadataAvailable = false;
    configuredInProgressSteps_ = 0;
    metadataBeginCalls_ = 0;
    metadataStepCalls_ = 0;
  }
  static size_t metadataBeginCalls() { return metadataBeginCalls_; }
  static size_t metadataStepCalls() { return metadataStepCalls_; }

 private:
  bool readingMetadata_ = false;
  size_t remainingSteps_ = 0;
  inline static BookMetadataCache::BookMetadata metadata{};
  inline static std::string thumbnailPath;
  inline static bool metadataAvailable = false;
  inline static size_t configuredInProgressSteps_ = 0;
  inline static size_t metadataBeginCalls_ = 0;
  inline static size_t metadataStepCalls_ = 0;
};
