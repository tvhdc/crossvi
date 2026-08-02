#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"

static_assert(FontStorageUtils::MAX_FAMILY_NAME_BYTES + 1 == CrossPointSettings::SD_FONT_FAMILY_NAME_CAPACITY,
              "Font family path contract must match persisted settings capacity");

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

bool FontInstaller::isValidFamilyName(const char* name) { return FontStorageUtils::isValidFamilyName(name); }

bool FontInstaller::isValidCpfontFilename(const char* name) { return FontStorageUtils::isValidCpfontFilename(name); }

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  if (!isValidFamilyName(familyName)) return false;
  // Reuse the family's existing root if installed; otherwise pick the
  // default-write root (hidden if no roots exist yet).
  const char* root = rootForFamily(familyName);

  if (!ensureRootDir(root)) return false;

  char dirPath[FontStorageUtils::FONT_PATH_CAPACITY];
  if (!buildFamilyPathAtRoot(root, familyName, dirPath, sizeof(dirPath))) {
    LOG_ERR("FONT", "Font family path is too long");
    return false;
  }

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::ensureRootDir(const char* root) {
  if (!root || root[0] == '\0') return false;
  if (Storage.exists(root)) return true;
  if (Storage.mkdir(root)) return true;
  LOG_ERR("FONT", "Failed to create fonts dir: %s", root);
  return false;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  // Use the production parser as the single format authority. load() performs
  // bounded header/TOC/section/glyph validation without loading bitmap payloads
  // into RAM, so malformed uploads fail before atomic publish.
  SdCardFont font;
  if (!font.load(path)) {
    LOG_ERR("FONT", "Invalid .cpfont: %s", path);
    return false;
  }
  return true;
}

bool FontInstaller::validateFamilyDirectory(const char* directory) {
  HalFile dir = Storage.open(directory);
  if (!dir || !dir.isDirectory()) return false;
  bool foundFont = false;
  char name[FontStorageUtils::MAX_CPFONT_FILENAME_BYTES + 1];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(name, sizeof(name));
    entry.close();
    const size_t length = strlen(name);
    if (length < 7 || strcmp(name + length - 7, ".cpfont") != 0) continue;
    char path[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!isValidCpfontFilename(name) || !FontStorageUtils::buildFilePath(directory, name, path, sizeof(path)) ||
        !validateCpfontFile(path)) {
      dir.close();
      return false;
    }
    foundFont = true;
  }
  dir.close();
  return foundFont;
}

bool FontInstaller::recoverInterruptedFamilyDownload(const char* familyName) {
  if (!isValidFamilyName(familyName)) return false;
  const auto validateFamily = [](const char* directory, void* context) {
    return static_cast<FontInstaller*>(context)->validateFamilyDirectory(directory);
  };
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  return std::all_of(std::begin(roots), std::end(roots), [&](const char* root) {
    char finalDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    char stagingDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    char backupDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!buildFamilyPathAtRoot(root, familyName, finalDirectory, sizeof(finalDirectory)) ||
        !FontStorageUtils::buildTransactionDirectoryPath(root, familyName, ".download.tmp", stagingDirectory,
                                                         sizeof(stagingDirectory)) ||
        !FontStorageUtils::buildTransactionDirectoryPath(root, familyName, ".download.bak", backupDirectory,
                                                         sizeof(backupDirectory))) {
      return false;
    }
    if (!Storage.exists(stagingDirectory) && !Storage.exists(backupDirectory)) return true;
    return FontStorageUtils::recoverFamily(finalDirectory, stagingDirectory, backupDirectory, validateFamily, this,
                                           validateFamily, this) != FontStorageUtils::FamilyTransactionStatus::IoError;
  });
}

const char* FontInstaller::rootForFamily(const char* family) {
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  return root ? root : SdCardFontRegistry::defaultWriteRoot();
}

bool FontInstaller::buildFamilyPathAtRoot(const char* root, const char* family, char* outBuf, const size_t outBufSize) {
  if (!isValidFamilyName(family)) return false;
  return FontStorageUtils::buildFamilyPath(root, family, outBuf, outBufSize);
}

bool FontInstaller::buildFontPathAtRoot(const char* root, const char* family, const char* filename, char* outBuf,
                                        const size_t outBufSize) {
  if (!isValidFamilyName(family) || !isValidCpfontFilename(filename)) return false;
  return FontStorageUtils::buildFontPath(root, family, filename, outBuf, outBufSize);
}

bool FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, const size_t outBufSize) {
  // Use the same root selection as ensureFamilyDir: existing install dir wins,
  // otherwise the default-write root.
  return buildFontPathAtRoot(rootForFamily(family), family, filename, outBuf, outBufSize);
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    return Error::INVALID_FAMILY_NAME;
  }

  // A family may exist in either root (or, edge case, both). Remove from both.
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  bool removedAny = false;
  bool sawAny = false;
  for (const char* root : roots) {
    char dirPath[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!buildFamilyPathAtRoot(root, familyName, dirPath, sizeof(dirPath))) return Error::INVALID_FAMILY_NAME;
    if (!Storage.exists(dirPath)) continue;
    sawAny = true;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
    removedAny = true;
  }

  if (!sawAny) {
    LOG_DBG("FONT", "Family not found in any fonts root: %s", familyName);
    return Error::OK;  // Already gone
  }
  (void)removedAny;

  // If this was the active font, clear the setting
  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("FONT", "Cleared active SD font (deleted family: %s)", familyName);
  }

  return Error::OK;
}

void FontInstaller::refreshRegistry() { registry_.discover(); }

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return registry_.findFamily(familyName) != nullptr;
}
