#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long BOOK_ACTION_LONG_PRESS_MS = 500;
constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr size_t FILE_SCAN_ENTRIES_PER_TICK = 8;
constexpr size_t MAX_FILE_ENTRIES = 256;
constexpr size_t MAX_FILE_NAME_BYTES = 24U * 1024U;
constexpr uint32_t SOURCE_PREPARATION_IDLE_MS = 250;

bool isPinnableBookPath(const std::string_view path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}
}  // namespace

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode)
    : Activity("FileBrowser", renderer, mappedInput),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}

FileBrowserActivity::~FileBrowserActivity() = default;

void FileBrowserActivity::cancelFileLoad() {
  if (fileLoadDirectory) fileLoadDirectory.close();
  filesLoading = false;
  fileLoadFrameRendered = false;
  fileLoadBackPending = false;
  fileLoadBackHeldMs = 0;
  pendingSelectionName.clear();
  pendingSelectionIndex = static_cast<size_t>(-1);
}

void FileBrowserActivity::loadFiles(std::string selectionName, const size_t selectionIndex) {
  cancelFileLoad();
  preparedEpub.reset();
  sourcePreparationFailedPath.clear();
  clearSearch();
  files.clear();
  filesTruncated = false;
  fileNameBytes = 0;
  pendingSelectionName = std::move(selectionName);
  pendingSelectionIndex = selectionIndex;
  selectorIndex = 0;

  fileLoadDirectory = Storage.open(basepath.c_str());
  if (!fileLoadDirectory || !fileLoadDirectory.isDirectory()) {
    if (fileLoadDirectory) fileLoadDirectory.close();
    requestUpdate();
    return;
  }

  fileLoadDirectory.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    fileLoadDirectory.close();
    requestUpdate();
    return;
  }

  filesLoading = true;
  requestUpdate();
}

void FileBrowserActivity::finishFileLoad() {
  if (fileLoadDirectory) fileLoadDirectory.close();
  FsHelpers::sortFileList(files);
  filesLoading = false;
  fileLoadFrameRendered = false;

  if (!pendingSelectionName.empty()) {
    selectorIndex = findEntry(pendingSelectionName);
  } else if (pendingSelectionIndex != static_cast<size_t>(-1)) {
    const size_t count = visibleItemCount();
    selectorIndex = count == 0 ? 0 : std::min(pendingSelectionIndex, count - 1);
  } else {
    selectorIndex = mode == Mode::Books && !files.empty() ? 1 : 0;
  }
  pendingSelectionName.clear();
  pendingSelectionIndex = static_cast<size_t>(-1);
}

bool FileBrowserActivity::stepFileLoad(const size_t maxEntries) {
  if (!filesLoading || !fileLoadDirectory || maxEntries == 0) return false;

  for (size_t scanned = 0; scanned < maxEntries; ++scanned) {
    HalFile file = fileLoadDirectory.openNextFile();
    if (!file) {
      const uint8_t error = fileLoadDirectory.getError();
      if (error != 0) {
        LOG_ERR("FileBrowser", "Directory scan failed with SD error %u", error);
        cancelFileLoad();
        files.clear();
        fileNameBytes = 0;
        selectorIndex = 0;
        popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
        popupTime = millis();
        return true;
      }
      finishFileLoad();
      return true;
    }

    fileNameBuffer[0] = '\0';
    const size_t nameLength = file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    file.close();
    if (nameLength == 0 || nameLength >= NAME_BUFFER_SIZE || fileNameBuffer[NAME_BUFFER_SIZE - 1] != '\0') continue;

    if (isBookFileTransactionArtifact(fileNameBuffer.get()) ||
        (!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    const std::string_view filename{fileNameBuffer.get()};
    bool supported = isDirectory;
    if (!isDirectory && mode == Mode::PickFirmware) {
      supported = FsHelpers::checkFileExtension(filename, ".bin");
    } else if (!isDirectory && mode == Mode::PickImage) {
      supported = FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
    } else if (!isDirectory && mode == Mode::PickVocabulary) {
      supported = FsHelpers::checkFileExtension(filename, ".cvocab");
    } else if (!isDirectory) {
      supported = FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                  FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                  FsHelpers::hasBmpExtension(filename);
    }
    if (!supported) continue;

    const size_t storedNameBytes = filename.size() + (isDirectory ? 1U : 0U) + 1U;
    if (files.size() >= MAX_FILE_ENTRIES || storedNameBytes > MAX_FILE_NAME_BYTES - fileNameBytes) {
      filesTruncated = true;
      finishFileLoad();
      return true;
    }

    if (isDirectory) {
      files.emplace_back(std::string(filename) + "/");
    } else {
      files.emplace_back(filename);
    }
    fileNameBytes += storedNameBytes;
  }
  return false;
}

void FileBrowserActivity::clearSearch(const bool preserveQuery) {
  searchActive = false;
  searchResultsTruncated = false;
  if (!preserveQuery) searchQuery.clear();
  searchResults.clear();
}

size_t FileBrowserActivity::visibleItemCount() const {
  if (filesLoading) return 0;
  if (mode != Mode::Books) return files.size();
  return searchActive ? searchResults.size() : files.size() + 1;
}

bool FileBrowserActivity::isSearchRow(const size_t index) const {
  return mode == Mode::Books && !searchActive && index == 0;
}

const std::string* FileBrowserActivity::visibleEntry(const size_t index) const {
  if (mode != Mode::Books) return index < files.size() ? &files[index] : nullptr;
  if (searchActive) {
    if (index >= searchResults.size() || searchResults[index] >= files.size()) return nullptr;
    return &files[searchResults[index]];
  }
  if (index == 0 || index - 1 >= files.size()) return nullptr;
  return &files[index - 1];
}

void FileBrowserActivity::launchSearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_BOOKS),
                         searchQuery, BOOK_SEARCH_QUERY_BYTES),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             lockLongPressBack = true;
                             return;
                           }
                           RenderLock lock(*this);
                           applySearch(std::get<KeyboardResult>(result.data).text);
                         });
}

void FileBrowserActivity::applySearch(const std::string& query) {
  const BookSearchQuery normalized = makeBookSearchQuery(query);
  if (normalized.empty()) {
    clearSearch();
    selectorIndex = files.empty() ? 0 : 1;
    return;
  }

  searchActive = true;
  searchQuery = query;
  searchResultsTruncated = false;
  searchResults.clear();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int footerReserved = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int pageCapacity = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, footerReserved);
  const size_t maxResults =
      static_cast<size_t>(std::clamp(pageCapacity, 1, static_cast<int>(BOOK_SEARCH_RESULT_HARD_LIMIT)));
  searchResults.reserve(maxResults);
  size_t exactCount = 0;

  const auto& recent = RECENT_BOOKS.getBooks();
  for (size_t i = 0; i < files.size(); i++) {
    const std::string& entry = files[i];
    if (entry.empty() || entry.back() == '/' || !isPinnableBookPath(entry)) continue;

    BookSearchMatch rank = matchBookSearch(normalized, entry);
    std::string fullPath = basepath;
    if (fullPath.back() != '/') fullPath += '/';
    fullPath += entry;
    const auto metadata = std::find_if(recent.begin(), recent.end(),
                                       [&fullPath](const RecentBook& book) { return book.path == fullPath; });
    if (metadata != recent.end()) {
      rank = std::max(rank, matchBookSearch(normalized, metadata->title));
      rank = std::max(rank, matchBookSearch(normalized, metadata->author));
    }
    addRankedBookSearchResult(searchResults, exactCount, searchResultsTruncated, i, rank, maxResults);
  }
  selectorIndex = 0;
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;
  sourcePreparationLastInputAt = static_cast<uint32_t>(millis());

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    root.close();
    basepath = FsHelpers::extractFolderPath(basepath);

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    loadFiles(fileName);
  } else {
    root.close();
    loadFiles();
  }
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  cancelFileLoad();
  files.clear();
  fileNameBuffer.reset();
}

bool FileBrowserActivity::skipLoopDelay() {
  return filesLoading || (preparedEpub && preparedEpub->isReadingCoreMetadata());
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    if (!canDeleteOrRelocateBookFile(fullPath)) return false;
    if (!Storage.remove(fullPath.c_str())) return false;
    removeBookUserStateAfterDelete(fullPath);
    return true;
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      fileNameBuffer[0] = '\0';
      const size_t nameLength = entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (nameLength == 0 || nameLength >= NAME_BUFFER_SIZE || fileNameBuffer[nameLength] != '\0') {
        LOG_ERR("FileBrowser", "Failed to read an entry name in: %s", currentPath.c_str());
        entry.close();
        dir.close();
        return false;
      }
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        if (!canDeleteOrRelocateBookFile(entryPath)) {
          LOG_ERR("FileBrowser", "Refusing to delete book with pending statistics: %s", entryPath.c_str());
          return false;
        }
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
        removeBookUserStateAfterDelete(entryPath);
      }
    }

    const uint8_t scanError = dir.getError();
    const bool closeSucceeded = dir.close();
    if (scanError != 0 || !closeSucceeded) {
      LOG_ERR("FileBrowser", "Directory iteration failed while deleting: %s", currentPath.c_str());
      return false;
    }
  }

  return true;
}

void FileBrowserActivity::promptDelete(const std::string& fullPath, const std::string& entry) {
  auto handler = [this, fullPath](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!removeDirFile(fullPath)) {
      LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
      return;
    }
    RenderLock lock(*this);
    loadFiles({}, selectorIndex);
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), entry),
      std::move(handler));
}

void FileBrowserActivity::showBookActions(const std::string& fullPath, const std::string& entry,
                                          const bool isDirectory) {
  const bool pinnable = !isDirectory && isPinnableBookPath(fullPath);
  std::vector<std::string> options;
  if (pinnable) options.push_back(RECENT_BOOKS.isPinned(fullPath) ? tr(STR_UNPIN_BOOK) : tr(STR_PIN_BOOK));
  options.push_back(tr(STR_DELETE));

  optionPopup.show(StrId::STR_BOOK_ACTIONS, std::move(options), 0,
                   [this, fullPath, entry, pinnable](const int selected) {
                     if (pinnable && selected == 0) {
                       const auto result = RECENT_BOOKS.togglePin(fullPath);
                       if (result == RecentBooksStore::PinResult::Pinned) {
                         popupMessage = StrId::STR_BOOK_PINNED;
                       } else if (result == RecentBooksStore::PinResult::Unpinned) {
                         popupMessage = StrId::STR_BOOK_UNPINNED;
                       } else if (result == RecentBooksStore::PinResult::LimitReached) {
                         popupMessage = StrId::STR_PIN_LIMIT_REACHED;
                       } else {
                         popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
                       }
                       popupTime = millis();
                       return;
                     }
                     promptDelete(fullPath, entry);
                   });
  requestUpdate();
}

void FileBrowserActivity::openPreparedBook(const std::string& path) {
  RawSourceIdentityHandoff preparedIdentity;
  if (preparedEpub && preparedEpub->getPath() == path && preparedEpub->getSourceIdentityHandoff(preparedIdentity)) {
    openBookWithFeedback(std::move(preparedEpub), ReaderOpenOrigin::Default);
    return;
  }
  onSelectBook(path);
}

void FileBrowserActivity::processSelectedSourcePreparation() {
  if (mode != Mode::Books || filesLoading || optionPopup.isActive() || popupMessage != StrId::STR_NONE_OPT ||
      mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
      mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.isPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
      static_cast<uint32_t>(millis() - sourcePreparationLastInputAt) < SOURCE_PREPARATION_IDLE_MS) {
    return;
  }

  const std::string* entry = visibleEntry(selectorIndex);
  if (!entry || entry->empty() || entry->back() == '/') {
    preparedEpub.reset();
    return;
  }
  std::string path = basepath;
  if (path.back() != '/') path += '/';
  path += *entry;
  if (sourcePreparationFailedPath == path) return;

  if (preparedEpub && preparedEpub->getPath() != path) preparedEpub.reset();

  if (!FsHelpers::hasEpubExtension(path)) return;
  if (!preparedEpub || preparedEpub->getPath() != path) {
    preparedEpub.reset(new (std::nothrow) Epub(path, "/.crosspoint"));
    if (!preparedEpub) {
      sourcePreparationFailedPath = path;
      return;
    }
  }
  if (preparedEpub->hasPreparedCoreMetadata()) return;
  if (!preparedEpub->isReadingCoreMetadata() && !preparedEpub->beginCoreMetadataRead()) {
    preparedEpub.reset();
    sourcePreparationFailedPath = path;
    return;
  }
  BookMetadataCache::BookMetadata metadata;
  if (preparedEpub->stepCoreMetadataRead(metadata) == Epub::CoreMetadataStepResult::Error) {
    preparedEpub.reset();
    sourcePreparationFailedPath = path;
  }
}

void FileBrowserActivity::loop() {
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
    sourcePreparationLastInputAt = static_cast<uint32_t>(millis());
  }
  if (optionPopup.isActive() && suppressPopupConfirmRelease &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    suppressPopupConfirmRelease = false;
    confirmPressSeen = false;
    confirmLongHandled = false;
    return;
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (popupMessage != StrId::STR_NONE_OPT) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popupMessage = StrId::STR_NONE_OPT;
      requestUpdate();
    }
    return;
  }

  if (filesLoading) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      fileLoadBackPending = true;
      fileLoadBackHeldMs = mappedInput.getHeldTime(MappedInputManager::Button::Back);
    }

    RenderLock lock(std::try_to_lock);
    if (!lock.ownsLock()) return;

    if (fileLoadBackPending) {
      const unsigned long heldMs = fileLoadBackHeldMs;
      fileLoadBackPending = false;
      fileLoadBackHeldMs = 0;
      if (lockLongPressBack) {
        lockLongPressBack = false;
        return;
      }
      cancelFileLoad();
      if (mode == Mode::Books && heldMs >= GO_HOME_MS && basepath != "/") {
        basepath = "/";
        loadFiles();
      } else if (basepath != "/") {
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        const auto pos = oldPath.find_last_of('/');
        loadFiles(oldPath.substr(pos + 1) + "/");
      } else if (mode != Mode::Books) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
      } else {
        onGoHome();
      }
      return;
    }

    if (!fileLoadFrameRendered) return;
    const bool completed = stepFileLoad(FILE_SCAN_ENTRIES_PER_TICK);
    if (completed) requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen = true;
    confirmLongHandled = false;
  }

  if (mode == Mode::Books && confirmPressSeen && !confirmLongHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Confirm) >= BOOK_ACTION_LONG_PRESS_MS &&
      !isSearchRow(selectorIndex)) {
    const std::string* selectedEntry = visibleEntry(selectorIndex);
    if (selectedEntry) {
      const std::string& entry = *selectedEntry;
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += '/';
      confirmLongHandled = true;
      suppressPopupConfirmRelease = true;
      showBookActions(cleanBasePath + entry, entry, entry.back() == '/');
      return;
    }
  }

  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Back) >= GO_HOME_MS && basepath != "/" &&
      !lockLongPressBack) {
    RenderLock lock(*this);
    lockLongPressBack = true;
    basepath = "/";
    loadFiles();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      confirmPressSeen = false;
      confirmLongHandled = false;
      return;
    }
    if (confirmLongHandled) {
      confirmPressSeen = false;
      confirmLongHandled = false;
      suppressPopupConfirmRelease = false;
      return;
    }
    const bool accept = confirmPressSeen;
    confirmPressSeen = false;
    if (!accept) return;
    if (isSearchRow(selectorIndex)) {
      launchSearch();
      return;
    }
    RenderLock lock(*this);
    const std::string* selectedEntry = visibleEntry(selectorIndex);
    if (!selectedEntry) return;

    const std::string entry = *selectedEntry;
    const bool isDirectory = (entry.back() == '/');

    // Picker modes: select file -> return path; navigate into directories normally.
    if (mode != Mode::Books && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      lock.unlock();
      setResult(std::move(res));
      finish();
      return;
    }

    // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
    if (basepath.back() != '/') basepath += "/";

    if (isDirectory) {
      basepath += entry.substr(0, entry.length() - 1);
      loadFiles();
    } else {
      const std::string fullPath = basepath + entry;
      lock.unlock();
      openPreparedBook(fullPath);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (searchActive) {
      {
        RenderLock lock(*this);
        clearSearch(true);
        selectorIndex = files.empty() ? 0 : 1;
      }
      requestUpdate();
      return;
    }
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime(MappedInputManager::Button::Back) < GO_HOME_MS) {
      if (basepath != "/") {
        RenderLock lock(*this);
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        loadFiles(dirName);
      } else if (mode != Mode::Books) {
        // Picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(visibleItemCount());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  processSelectedSourcePreparation();
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  if (renderBookLoadingOverlay()) return;
  if (optionPopup.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName;
  if (searchActive) {
    char searchTitle[BOOK_SEARCH_QUERY_BYTES + 96]{};
    snprintf(searchTitle, sizeof(searchTitle), tr(STR_SEARCH_RESULTS_FORMAT), searchQuery.c_str());
    folderName = searchTitle;
  } else if (mode == Mode::PickFirmware) {
    folderName = tr(STR_SELECT_FIRMWARE_FILE);
  } else if (mode == Mode::PickImage) {
    folderName = tr(STR_SLEEP_SCREEN);
  } else if (mode == Mode::PickVocabulary) {
    folderName = tr(STR_VOCAB_SELECT_FILE);
  } else {
    folderName = basepath == "/" ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1);
  }
  if (filesTruncated) folderName += " (" + std::to_string(files.size()) + "+)";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  const size_t itemCount = visibleItemCount();
  if (filesLoading) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_LOADING));
  } else if (itemCount == 0) {
    const char* emptyMsg = searchActive ? tr(STR_NO_SEARCH_RESULTS)
                                        : (mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectorIndex,
        [this](int index) {
          if (isSearchRow(index)) return std::string(tr(STR_SEARCH_BOOKS));
          const std::string* entry = visibleEntry(index);
          return entry ? getFileName(*entry) : std::string{};
        },
        nullptr,
        [this](int index) {
          if (isSearchRow(index)) return UIIcon::Search;
          const std::string* entry = visibleEntry(index);
          return entry ? UITheme::getFileIcon(*entry) : UIIcon::None;
        },
        [this](int index) {
          const std::string* entry = visibleEntry(index);
          return entry ? getFileExtension(*entry) : std::string{};
        },
        false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    if (searchActive && searchResultsTruncated) {
      const std::string notice = renderer.truncatedText(SMALL_FONT_ID, tr(STR_SEARCH_MORE_RESULTS), pathMaxWidth);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, notice.c_str());
    } else {
      // Left-truncate so the deepest directory is always visible
      const char* pathStr = basepath.c_str();
      const char* pathDisplay = pathStr;
      char leftTruncBuf[256];
      if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
        const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
        const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
        const int available = pathMaxWidth - ellipsisWidth;
        // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
        const char* p = pathStr;
        while (*p) {
          if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
          ++p;
          while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
        }
        snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
        pathDisplay = leftTruncBuf;
      }
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
    }
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode != Mode::Books ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In picker modes, Confirm on a file returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const std::string* selectedEntry = visibleEntry(selectorIndex);
  const bool selectingPickerFile = mode != Mode::Books && selectedEntry && selectedEntry->back() != '/';
  const char* confirmLabel =
      itemCount == 0
          ? ""
          : (isSearchRow(selectorIndex) ? tr(STR_SEARCH) : (selectingPickerFile ? tr(STR_SELECT) : tr(STR_OPEN)));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, itemCount == 0 ? "" : tr(STR_DIR_UP),
                                            itemCount == 0 ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (popupMessage != StrId::STR_NONE_OPT) {
    GUI.drawPopup(renderer, I18N.get(popupMessage));
  } else {
    renderer.displayBuffer();
  }
  if (filesLoading) fileLoadFrameRendered = true;
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return mode == Mode::Books ? i + 1 : i;
  return 0;
}
