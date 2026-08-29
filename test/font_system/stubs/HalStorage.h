#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

struct TestStorageEntry {
  std::string path;
  std::string name;
  bool directory = false;
  std::vector<std::string> children;
  size_t failIterationAfter = std::numeric_limits<size_t>::max();
};

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path) {
    stream_.open(path, std::ios::binary);
    if (!stream_) return false;
    stream_.seekg(0, std::ios::end);
    size_ = static_cast<uint64_t>(stream_.tellg());
    stream_.seekg(0, std::ios::beg);
    return true;
  }

  explicit operator bool() const { return testOpen_ || stream_.is_open(); }
  bool isDirectory() const { return testOpen_ && testEntry_ && testEntry_->directory; }
  HalFile openNextFile();
  void getName(char* destination, const size_t capacity) const {
    if (capacity == 0) return;
    if (!testOpen_ || !testEntry_) {
      destination[0] = '\0';
      return;
    }
    std::strncpy(destination, testEntry_->name.c_str(), capacity - 1);
    destination[capacity - 1] = '\0';
  }

  int read(void* destination, size_t length) {
    if (!stream_) return 0;
    stream_.read(static_cast<char*>(destination), static_cast<std::streamsize>(length));
    return static_cast<int>(stream_.gcount());
  }

  bool seekSet(size_t position) { return seek64(position); }
  bool seek64(uint64_t position) {
    if (!stream_.is_open() || position > size_) return false;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    return static_cast<bool>(stream_);
  }

  uint64_t fileSize64() const { return size_; }
  size_t fileSize() const { return static_cast<size_t>(size_); }
  uint8_t getError() const { return error_; }
  bool close();

 private:
  friend class HalStorage;
  HalFile(TestStorageEntry* entry, HalStorage* storage) : testEntry_(entry), storage_(storage), testOpen_(true) {}

  std::ifstream stream_;
  uint64_t size_ = 0;
  TestStorageEntry* testEntry_ = nullptr;
  HalStorage* storage_ = nullptr;
  size_t nextChild_ = 0;
  uint8_t error_ = 0;
  bool testOpen_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path); }
  HalFile open(const char* path) {
    const auto entry = entries_.find(path);
    if (entry == entries_.end() || failOpenPath_ == path) return {};
    return HalFile(&entry->second, this);
  }
  bool exists(const char* path) const { return entries_.find(path) != entries_.end(); }
  bool ready() const { return ready_; }

  void resetTestTree() {
    entries_.clear();
    failOpenPath_.clear();
    failClosePath_.clear();
    ready_ = true;
  }

  void addTestDirectory(const char* path) { addTestEntry(path, true); }
  void addTestFile(const char* path) { addTestEntry(path, false); }
  void failDirectoryIterationAfter(const char* path, const size_t count) {
    entries_.at(path).failIterationAfter = count;
  }
  void failOpen(const char* path) { failOpenPath_ = path; }
  void failClose(const char* path) { failClosePath_ = path; }
  void setReady(const bool ready) { ready_ = ready; }

 private:
  friend class HalFile;

  void addTestEntry(const std::string& path, const bool directory) {
    const size_t slash = path.find_last_of('/');
    TestStorageEntry entry;
    entry.path = path;
    entry.name = slash == std::string::npos ? path : path.substr(slash + 1);
    entry.directory = directory;
    entries_[path] = std::move(entry);
    if (slash > 0) {
      const std::string parent = path.substr(0, slash);
      auto parentEntry = entries_.find(parent);
      if (parentEntry != entries_.end()) parentEntry->second.children.push_back(path);
    }
  }

  bool shouldFailClose(const std::string& path) const { return failClosePath_ == path; }

  std::map<std::string, TestStorageEntry> entries_;
  std::string failOpenPath_;
  std::string failClosePath_;
  bool ready_ = true;
};

inline HalFile HalFile::openNextFile() {
  if (!testOpen_ || !testEntry_ || !testEntry_->directory || !storage_) return {};
  if (nextChild_ >= testEntry_->failIterationAfter) {
    error_ = 1;
    return {};
  }
  if (nextChild_ >= testEntry_->children.size()) return {};
  const auto child = storage_->entries_.find(testEntry_->children[nextChild_++]);
  if (child == storage_->entries_.end()) return {};
  return HalFile(&child->second, storage_);
}

inline bool HalFile::close() {
  stream_.close();
  const bool succeeded = !testOpen_ || !storage_ || !storage_->shouldFailClose(testEntry_->path);
  testOpen_ = false;
  return succeeded;
}

#define Storage HalStorage::getInstance()
