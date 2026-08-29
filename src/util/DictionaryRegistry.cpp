#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "StringUtils.h"
#include "Utf8.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};

template <size_t Capacity>
bool readValidEntryName(HalFile& entry, char (&name)[Capacity]) {
  name[0] = '\0';
  const size_t length = entry.getName(name, Capacity);
  return length > 0 && length < Capacity && name[length] == '\0' && strnlen(name, Capacity) == length &&
         utf8IsValid(std::string_view(name, length));
}

enum class FindStemStatus : uint8_t { Found, NotFound, IoError };

// Find the single .idx stem inside one dictionary folder. An interrupted
// directory traversal must not make the first visible stem look unique.
FindStemStatus findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return FindStemStatus::NotFound;

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!readValidEntryName(entry, name)) continue;
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_DBG("DREG", "Skipping %s: multiple index stems found", folderPath);
      return FindStemStatus::NotFound;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  const bool iterationSucceeded = dir.getError() == 0;
  const bool directoryClosed = dir.close();
  if (!iterationSucceeded || !directoryClosed) {
    LOG_ERR("DREG", "Could not completely scan %s", folderPath);
    return FindStemStatus::IoError;
  }

  if (foundStem[0] == '\0') return FindStemStatus::NotFound;

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", folderPath);
    return FindStemStatus::NotFound;
  }

  stemOut = foundStem;
  return FindStemStatus::Found;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  std::vector<DictionaryEntry> discovered;
  discovered.reserve(8);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    // The selected folder name is persisted verbatim in SETTINGS.dictionaryName.
    // A smaller buffer makes SdFat reject names that cannot round-trip through settings.
    char name[sizeof(SETTINGS.dictionaryName)];
    bool scanFailed = false;
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      if (!readValidEntryName(entry, name)) continue;
      if (!entry.isDirectory() || name[0] == '.') continue;

      // A dictionary can exist in both the visible and hidden roots. The
      // persisted selection stores only the folder name, and resolveBasePath()
      // always uses the first matching root, so listing the later copy would
      // create a duplicate menu row that can never select a different file.
      const bool alreadyListed =
          std::any_of(discovered.begin(), discovered.end(),
                      [name](const DictionaryEntry& dictionary) { return dictionary.name == name; });
      if (alreadyListed) continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      std::string stem;
      const FindStemStatus stemStatus = findStem(folderPath.c_str(), stem);
      if (stemStatus == FindStemStatus::IoError) {
        scanFailed = true;
        break;
      }
      if (stemStatus != FindStemStatus::Found) continue;

      DictionaryEntry e;
      e.name = name;
      e.stem = std::move(stem);
      discovered.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary: %s", name);
    }

    const bool iterationSucceeded = rootDir.getError() == 0;
    const bool directoryClosed = rootDir.close();
    if (scanFailed || !iterationSucceeded || !directoryClosed) {
      LOG_ERR("DREG", "Could not completely scan %s", dictRoot);
      return;
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(discovered.begin(), discovered.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  out.swap(discovered);
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  const size_t length = strnlen(folderName, sizeof(SETTINGS.dictionaryName));
  if (length == sizeof(SETTINGS.dictionaryName) || !utf8IsValid(std::string_view(folderName, length))) return false;
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape the dictionary roots.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    std::string stem;
    if (findStem(folderPath.c_str(), stem) != FindStemStatus::Found) continue;
    basePathOut = folderPath + "/" + stem;
    return true;
  }
  return false;
}

}  // namespace DictionaryRegistry
