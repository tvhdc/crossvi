#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>

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

  explicit operator bool() const { return stream_.is_open(); }
  bool isDirectory() const { return false; }
  HalFile openNextFile() { return {}; }
  void getName(char* destination, const size_t capacity) const {
    if (capacity > 0) destination[0] = '\0';
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
  bool close() {
    stream_.close();
    return true;
  }

 private:
  std::ifstream stream_;
  uint64_t size_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path); }
  HalFile open(const char*) { return {}; }
  bool exists(const char*) const { return false; }
};

#define Storage HalStorage::getInstance()
