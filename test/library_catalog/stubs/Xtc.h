#pragma once

#include <string>
#include <utility>

class Xtc {
 public:
  Xtc(const std::string&, const std::string&) {}
  bool load() {
    ++loadCalls_;
    return false;
  }
  bool readCoreMetadata(std::string& title, std::string& author) const {
    ++metadataReadCalls_;
    if (!metadataAvailable_) return false;
    title = title_;
    author = author_;
    return true;
  }
  std::string getTitle() const { return title_; }
  std::string getAuthor() const { return author_; }
  std::string getThumbBmpPath() const { return thumbnailPath_; }

  static void setMetadata(std::string title, std::string author, std::string thumbnail) {
    title_ = std::move(title);
    author_ = std::move(author);
    thumbnailPath_ = std::move(thumbnail);
    metadataAvailable_ = true;
  }
  static void resetMetadata() {
    title_.clear();
    author_.clear();
    thumbnailPath_.clear();
    metadataAvailable_ = false;
    metadataReadCalls_ = 0;
    loadCalls_ = 0;
  }
  static size_t metadataReadCalls() { return metadataReadCalls_; }
  static size_t loadCalls() { return loadCalls_; }

 private:
  inline static std::string title_;
  inline static std::string author_;
  inline static std::string thumbnailPath_;
  inline static bool metadataAvailable_ = false;
  inline static size_t metadataReadCalls_ = 0;
  inline static size_t loadCalls_ = 0;
};
