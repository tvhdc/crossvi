#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

class Print {
 public:
  virtual ~Print() = default;
#if defined(CROSSPOINT_TEST_PURE_SINGLE_BYTE_PRINT)
  virtual size_t write(uint8_t value) = 0;
#else
  virtual size_t write(const uint8_t value) { return write(&value, 1); }
#endif
  virtual size_t write(const uint8_t*, size_t length) { return length; }
};

class HalStorage;

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t size() const;
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  bool isOpen() const { return open_; }
  bool seek(size_t position);
  bool seekSet(size_t position) { return seek(position); }
  bool seek64(uint64_t position) {
    return position <= static_cast<uint64_t>(SIZE_MAX) && seek(static_cast<size_t>(position));
  }
  bool seekCur(int64_t offset);
  int available() const;
  size_t position() const;
  bool getModifyDateTime(uint16_t* date, uint16_t* time) const;
  uint8_t getError() const { return error_ ? 1 : 0; }
  size_t getName(char* name, size_t length) const;
  bool isDirectory() const { return open_ && directory_; }
  HalFile openNextFile();
  int read(void* destination, size_t length);
  int read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
  }
  using Print::write;
  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* source, size_t length) override;
  size_t write(const void* source, size_t length);
  void flush() {}
  bool sync();
  bool close();
  explicit operator bool() const { return open_; }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  size_t position_ = 0;
  bool writable_ = false;
  bool open_ = false;
  bool error_ = false;
  bool directory_ = false;
  size_t directoryIndex_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) const {
    ++existsAttempts_[path];
    return files_.count(path) != 0 || directories_.count(path) != 0;
  }
  bool mkdir(const char* path, bool = true) {
    if (exists(path)) return false;
    directories_.insert(path);
    return true;
  }
  bool ensureDirectoryExists(const char* path) { return exists(path) || mkdir(path); }
  bool probeMedia() const { return true; }
  bool remove(const char* path) {
    ++removeAttempts_[path];
    if (openPathCounts_.count(path) != 0) ++removeWhileOpenAttempts_[path];
    if (failRemovePath_ == path) {
      failRemovePath_.clear();
      return false;
    }
    modified_.erase(path);
    return files_.erase(path) != 0;
  }
  bool removeDir(const char* path) {
    if (failRemoveDir_) {
      failRemoveDir_ = false;
      return false;
    }
    const std::string root = path;
    const std::string prefix = root + "/";
    bool removed = directories_.erase(root) != 0;
    for (auto it = files_.begin(); it != files_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) {
        it = files_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    for (auto it = directories_.begin(); it != directories_.end();) {
      if (it->compare(0, prefix.size(), prefix) == 0) {
        it = directories_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    return removed;
  }
  bool rename(const char* from, const char* to) {
    if (failRename_ || failRenameDestination_ == to) {
      failRename_ = false;
      failRenameDestination_.clear();
      return false;
    }
    const auto found = files_.find(from);
    if (found == files_.end()) {
      const std::string sourceRoot = from;
      const std::string destinationRoot = to;
      if (directories_.count(sourceRoot) == 0 || exists(to)) return false;
      const std::string sourcePrefix = sourceRoot + "/";
      const std::string destinationPrefix = destinationRoot + "/";
      std::vector<std::pair<std::string, std::vector<uint8_t>>> movedFiles;
      std::vector<std::string> movedDirectories;
      for (const auto& item : files_) {
        if (item.first.compare(0, sourcePrefix.size(), sourcePrefix) == 0) {
          movedFiles.emplace_back(destinationPrefix + item.first.substr(sourcePrefix.size()), item.second);
        }
      }
      for (const auto& directory : directories_) {
        if (directory.compare(0, sourcePrefix.size(), sourcePrefix) == 0) {
          movedDirectories.push_back(destinationPrefix + directory.substr(sourcePrefix.size()));
        }
      }
      removeDir(sourceRoot.c_str());
      directories_.insert(destinationRoot);
      directories_.insert(movedDirectories.begin(), movedDirectories.end());
      for (auto& item : movedFiles) files_[item.first] = std::move(item.second);
      return true;
    }
    if (files_.count(to) != 0 || directories_.count(to) != 0) return false;
    files_[to] = found->second;
    const auto modified = modified_.find(from);
    if (modified != modified_.end()) {
      modified_[to] = modified->second;
      modified_.erase(modified);
    }
    files_.erase(found);
    if ((corruptRename_ || corruptRenameDestination_ == to) && !files_[to].empty()) {
      corruptRename_ = false;
      corruptRenameDestination_.clear();
      files_[to].back() ^= 0x80U;
    }
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    const size_t attempt = ++openReadAttempts_[path];
    const auto failAttempt = failOpenReadAttempt_.find(path);
    if (failAttempt != failOpenReadAttempt_.end() && failAttempt->second == attempt) {
      failOpenReadAttempt_.erase(failAttempt);
      return false;
    }
    if (unreadable_.count(path) || files_.count(path) == 0) return false;
    file = makeFile(path, false);
    return true;
  }
  bool openFileForRead(const char* tag, const std::string& path, HalFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    ++openWriteAttempts_[path];
    if (unwritable_.count(path)) return false;
    files_[path].clear();
    modified_[path] = ++modifiedClock_;
    file = makeFile(path, true);
    return true;
  }
  bool openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }
  HalFile open(const char* path, int flags = 0);

  void reset() {
    files_.clear();
    directories_.clear();
    unreadable_.clear();
    unwritable_.clear();
    shortWrite_ = false;
    shortWriteCall_ = 0;
    writeCalls_ = 0;
    shortWritePath_.clear();
    shortReadPath_.clear();
    failSync_ = false;
    failClosePath_.clear();
    failRename_ = false;
    failRenameDestination_.clear();
    failRemovePath_.clear();
    failRemoveDir_ = false;
    corruptRename_ = false;
    corruptRenameDestination_.clear();
    growOnReadCall_ = 0;
    readCalls_ = 0;
    seekCalls_ = 0;
    maxRead_ = 0;
    invalidOperations_ = 0;
    openReadAttempts_.clear();
    openWriteAttempts_.clear();
    existsAttempts_.clear();
    removeAttempts_.clear();
    openPathCounts_.clear();
    removeWhileOpenAttempts_.clear();
    failOpenReadAttempt_.clear();
    reportedSizes_.clear();
    modified_.clear();
    modifiedClock_ = 0;
  }
  void setFile(const std::string& path, std::vector<uint8_t> data) {
    files_[path] = std::move(data);
    modified_[path] = ++modifiedClock_;
  }
  void setDirectory(const std::string& path) { directories_.insert(path); }
  std::vector<uint8_t>& mutableFile(const std::string& path) { return files_.at(path); }
  const std::vector<uint8_t>& file(const std::string& path) const { return files_.at(path); }
  void makeUnreadable(const std::string& path) { unreadable_.insert(path); }
  void makeReadable(const std::string& path) { unreadable_.erase(path); }
  void makeUnwritable(const std::string& path) { unwritable_.insert(path); }
  void makeWritable(const std::string& path) { unwritable_.erase(path); }
  void shortWriteOnce() { shortWrite_ = true; }
  void shortWriteOnCall(const size_t call) {
    shortWriteCall_ = call;
    writeCalls_ = 0;
  }
  void shortWriteFor(const std::string& path) { shortWritePath_ = path; }
  void shortReadFor(const std::string& path) { shortReadPath_ = path; }
  void failSyncOnce() { failSync_ = true; }
  void failCloseFor(const std::string& path) { failClosePath_ = path; }
  void failRenameOnce() { failRename_ = true; }
  void failRenameTo(const std::string& destination) { failRenameDestination_ = destination; }
  void failRemoveFor(const std::string& path) { failRemovePath_ = path; }
  void failOpenReadOnAttempt(const std::string& path, const size_t attempt) { failOpenReadAttempt_[path] = attempt; }
  void failRemoveDirOnce() { failRemoveDir_ = true; }
  void corruptRenameOnce() { corruptRename_ = true; }
  void corruptRenameTo(const std::string& destination) { corruptRenameDestination_ = destination; }
  void growOnReadCall(size_t call) { growOnReadCall_ = call; }
  void reportFileSize(const std::string& path, const uint64_t size) { reportedSizes_[path] = size; }
  size_t maxRead() const { return maxRead_; }
  size_t readCalls() const { return readCalls_; }
  size_t seekCalls() const { return seekCalls_; }
  void resetIoCounters() {
    growOnReadCall_ = 0;
    readCalls_ = 0;
    seekCalls_ = 0;
    maxRead_ = 0;
    openReadAttempts_.clear();
    openWriteAttempts_.clear();
    existsAttempts_.clear();
    removeAttempts_.clear();
  }
  size_t invalidOperationCount() const { return invalidOperations_; }
  size_t openReadAttemptsFor(const std::string& path) const {
    const auto found = openReadAttempts_.find(path);
    return found == openReadAttempts_.end() ? 0 : found->second;
  }
  size_t openWriteAttemptsFor(const std::string& path) const {
    const auto found = openWriteAttempts_.find(path);
    return found == openWriteAttempts_.end() ? 0 : found->second;
  }
  size_t existsAttemptsFor(const std::string& path) const {
    const auto found = existsAttempts_.find(path);
    return found == existsAttempts_.end() ? 0 : found->second;
  }
  size_t removeAttemptsFor(const std::string& path) const {
    const auto found = removeAttempts_.find(path);
    return found == removeAttempts_.end() ? 0 : found->second;
  }
  size_t removeWhileOpenAttemptsFor(const std::string& path) const {
    const auto found = removeWhileOpenAttempts_.find(path);
    return found == removeWhileOpenAttempts_.end() ? 0 : found->second;
  }
  size_t openHandlesFor(const std::string& path) const {
    const auto found = openPathCounts_.find(path);
    return found == openPathCounts_.end() ? 0 : found->second;
  }

 private:
  friend class HalFile;
  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> directories_;
  std::set<std::string> unreadable_;
  std::set<std::string> unwritable_;
  bool shortWrite_ = false;
  size_t shortWriteCall_ = 0;
  size_t writeCalls_ = 0;
  std::string shortWritePath_;
  std::string shortReadPath_;
  bool failSync_ = false;
  std::string failClosePath_;
  bool failRename_ = false;
  std::string failRenameDestination_;
  std::string failRemovePath_;
  bool failRemoveDir_ = false;
  bool corruptRename_ = false;
  std::string corruptRenameDestination_;
  size_t growOnReadCall_ = 0;
  size_t readCalls_ = 0;
  size_t seekCalls_ = 0;
  size_t maxRead_ = 0;
  size_t invalidOperations_ = 0;
  std::map<std::string, size_t> openReadAttempts_;
  std::map<std::string, size_t> openWriteAttempts_;
  mutable std::map<std::string, size_t> existsAttempts_;
  std::map<std::string, size_t> removeAttempts_;
  std::map<std::string, size_t> openPathCounts_;
  std::map<std::string, size_t> removeWhileOpenAttempts_;
  std::map<std::string, size_t> failOpenReadAttempt_;
  std::map<std::string, uint64_t> reportedSizes_;
  std::map<std::string, uint32_t> modified_;
  uint32_t modifiedClock_ = 0;

  HalFile makeFile(const std::string& path, bool writable) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.open_ = true;
    ++openPathCounts_[path];
    return file;
  }

  HalFile makeDirectory(const std::string& path) {
    HalFile file = makeFile(path, false);
    file.directory_ = true;
    return file;
  }
};

inline size_t HalFile::size() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  const auto reported = storage_->reportedSizes_.find(path_);
  return reported == storage_->reportedSizes_.end() ? storage_->files_.at(path_).size()
                                                    : static_cast<size_t>(reported->second);
}

inline size_t HalFile::position() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  return position_;
}

inline bool HalFile::getModifyDateTime(uint16_t* date, uint16_t* time) const {
  if (!open_ || !storage_ || !date || !time) return false;
  const auto found = storage_->modified_.find(path_);
  if (found == storage_->modified_.end()) return false;
  *date = static_cast<uint16_t>(found->second >> 16U);
  *time = static_cast<uint16_t>(found->second);
  return true;
}

inline size_t HalFile::getName(char* name, const size_t length) const {
  if (!open_ || !storage_ || !name || length == 0) return 0;
  const size_t slash = path_.find_last_of('/');
  const std::string base = slash == std::string::npos ? path_ : path_.substr(slash + 1);
  if (base.size() + 1 > length) return 0;
  memcpy(name, base.c_str(), base.size() + 1);
  return base.size();
}

inline HalFile HalFile::openNextFile() {
  if (!open_ || !storage_ || !directory_) return {};
  std::string prefix = path_ == "/" ? "/" : path_ + "/";
  std::vector<std::string> children;
  for (const auto& [path, data] : storage_->files_) {
    static_cast<void>(data);
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string remainder = path.substr(prefix.size());
    if (!remainder.empty() && remainder.find('/') == std::string::npos) children.push_back(path);
  }
  for (const std::string& path : storage_->directories_) {
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string remainder = path.substr(prefix.size());
    if (!remainder.empty() && remainder.find('/') == std::string::npos) children.push_back(path);
  }
  std::sort(children.begin(), children.end());
  children.erase(std::unique(children.begin(), children.end()), children.end());
  if (directoryIndex_ >= children.size()) return {};
  const std::string child = children[directoryIndex_++];
  return storage_->directories_.count(child) ? storage_->makeDirectory(child) : storage_->makeFile(child, false);
}

inline bool HalFile::seek(const size_t position) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  if (position > size()) return false;
  ++storage_->seekCalls_;
  position_ = position;
  return true;
}

inline bool HalFile::seekCur(const int64_t offset) {
  if (offset < 0 && static_cast<uint64_t>(-offset) > position_) return false;
  const uint64_t next = offset < 0 ? position_ - static_cast<uint64_t>(-offset) : position_ + offset;
  return next <= size() && seek(static_cast<size_t>(next));
}

inline int HalFile::available() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  return position_ < size() ? static_cast<int>(size() - position_) : 0;
}

inline int HalFile::read(void* destination, const size_t length) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  if (writable_ || position_ >= size()) return 0;
  size_t readLength = std::min(length, size() - position_);
  if (storage_->shortReadPath_ == path_ && readLength > 0) {
    storage_->shortReadPath_.clear();
    --readLength;
    error_ = true;
  }
  storage_->maxRead_ = std::max(storage_->maxRead_, readLength);
  const auto& bytes = storage_->files_.at(path_);
  std::copy_n(bytes.data() + position_, readLength, static_cast<uint8_t*>(destination));
  position_ += readLength;
  ++storage_->readCalls_;
  if (storage_->growOnReadCall_ == storage_->readCalls_) storage_->files_[path_].push_back(0xA5U);
  return static_cast<int>(readLength);
}

inline size_t HalFile::write(const void* source, const size_t length) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  if (!writable_) return 0;
  ++storage_->writeCalls_;
  size_t written = length;
  if (storage_->shortWrite_ || storage_->shortWritePath_ == path_ ||
      (storage_->shortWriteCall_ != 0 && storage_->writeCalls_ == storage_->shortWriteCall_)) {
    storage_->shortWrite_ = false;
    storage_->shortWriteCall_ = 0;
    storage_->shortWritePath_.clear();
    written = length == 0 ? 0 : length - 1;
  }
  auto& bytes = storage_->files_[path_];
  if (position_ + written > bytes.size()) bytes.resize(position_ + written);
  std::copy_n(static_cast<const uint8_t*>(source), written, bytes.data() + position_);
  position_ += written;
  return written;
}

inline size_t HalFile::write(const uint8_t* source, const size_t length) {
  return write(static_cast<const void*>(source), length);
}

inline bool HalFile::sync() {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  if (storage_->failSync_) {
    storage_->failSync_ = false;
    return false;
  }
  return true;
}

inline bool HalFile::close() {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  const bool shouldFail = storage_->failClosePath_ == path_;
  if (shouldFail) storage_->failClosePath_.clear();
  const bool wasOpen = open_;
  open_ = false;
  const auto openPath = storage_->openPathCounts_.find(path_);
  if (openPath != storage_->openPathCounts_.end() && --openPath->second == 0) {
    storage_->openPathCounts_.erase(openPath);
  }
  return wasOpen && !shouldFail;
}

inline HalFile HalStorage::open(const char* path, const int flags) {
  if (flags == 0) {
    if (directories_.count(path) && unreadable_.count(path) == 0) return makeDirectory(path);
    if (files_.count(path) && unreadable_.count(path) == 0) return makeFile(path, false);
    return {};
  }
  files_[path].clear();
  return makeFile(path, true);
}

#define Storage HalStorage::getInstance()
