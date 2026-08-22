#include "NextBookFinder.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "BookPathMoveUtils.h"
#include "CrossPointSettings.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr size_t MAX_DIRECTORY_ENTRIES = 4096;

bool isSupportedBookFile(const std::string_view name) {
  // Formats ReaderActivity can open (bmp is a viewer, not a book, so it is excluded)
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}
}  // namespace

NextBookFinder::Scan::~Scan() {
  if (directory_) directory_.close();
}

bool NextBookFinder::Scan::begin(const std::string& currentBookPath, const size_t maxCount) {
  if (directory_) directory_.close();
  active_ = false;
  entriesScanned_ = 0;
  maxCount_ = maxCount;
  result_.clear();
  folder_.clear();
  currentName_.clear();
  nameBuffer_.reset();
  if (maxCount == 0 || currentBookPath.empty()) return false;

  folder_ = FsHelpers::extractFolderPath(currentBookPath);
  const auto lastSlash = currentBookPath.find_last_of('/');
  currentName_ = lastSlash == std::string::npos ? currentBookPath : currentBookPath.substr(lastSlash + 1);

  directory_ = Storage.open(folder_.c_str());
  if (!directory_ || !directory_.isDirectory()) {
    LOG_ERR("NBF", "Cannot open folder: %s", folder_.c_str());
    if (directory_) directory_.close();
    return false;
  }
  directory_.rewindDirectory();

  nameBuffer_ = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer_) {
    LOG_ERR("NBF", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    directory_.close();
    return false;
  }

  result_.reserve(maxCount + 1);
  active_ = true;
  return true;
}

NextBookFinder::StepResult NextBookFinder::Scan::step(const size_t maxEntries) {
  if (!active_) return StepResult::Complete;
  if (maxEntries == 0) return StepResult::Pending;
  const auto less = [](const std::string& a, const std::string& b) { return FsHelpers::naturalLess(a, b); };

  for (size_t processed = 0; processed < maxEntries && entriesScanned_ < MAX_DIRECTORY_ENTRIES; ++processed) {
    HalFile file = directory_.openNextFile();
    if (!file) {
      const bool ok = directory_.getError() == 0 && directory_.close();
      active_ = false;
      nameBuffer_.reset();
      return ok ? StepResult::Complete : StepResult::Error;
    }
    ++entriesScanned_;
    const bool isDirectory = file.isDirectory();
    const size_t length = isDirectory ? 0 : file.getName(nameBuffer_.get(), NAME_BUFFER_SIZE);
    const bool closed = file.close();
    if (!closed) {
      directory_.close();
      active_ = false;
      nameBuffer_.reset();
      return StepResult::Error;
    }
    if (length > 0 && length < NAME_BUFFER_SIZE) nameBuffer_[length] = '\0';
    if (isDirectory || length == 0 || length >= NAME_BUFFER_SIZE ||
        isBookFileTransactionArtifact(nameBuffer_.get()) ||
        (!SETTINGS.showHiddenFiles && nameBuffer_[0] == '.') || !isSupportedBookFile(nameBuffer_.get())) {
      continue;
    }
    std::string name{nameBuffer_.get(), length};
    if (!FsHelpers::naturalLess(currentName_, name) ||
        (result_.size() >= maxCount_ && !less(name, result_.back()))) {
      continue;
    }
    const auto pos = std::lower_bound(result_.begin(), result_.end(), name, less);
    result_.insert(pos, std::move(name));
    if (result_.size() > maxCount_) result_.pop_back();
  }

  if (entriesScanned_ >= MAX_DIRECTORY_ENTRIES) {
    const bool closed = directory_.close();
    active_ = false;
    nameBuffer_.reset();
    return closed ? StepResult::Complete : StepResult::Error;
  }
  return StepResult::Pending;
}
