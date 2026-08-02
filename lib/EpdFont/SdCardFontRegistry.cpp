#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "FontStorageContract.h"
#include "ReaderFontSize.h"

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findClosestPointSize(const uint8_t targetPointSize,
                                                                     const uint8_t style) const {
  if (files.empty()) return nullptr;

  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDelta = UINT8_MAX;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t delta = f.pointSize > targetPointSize ? f.pointSize - targetPointSize : targetPointSize - f.pointSize;
    if (!best || delta < bestDelta || (delta == bestDelta && f.pointSize < best->pointSize)) {
      best = &f;
      bestDelta = delta;
    }
  }
  return best;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findClosestReaderSize(const uint8_t fontSizeEnum,
                                                                      const uint8_t style) const {
  return findClosestPointSize(ReaderFontSize::pointSize(fontSizeEnum), style);
}

int SdCardFontFamilyInfo::findClosestReaderSizeEnum(const uint8_t targetPointSize, const uint8_t style) const {
  int bestEnum = -1;
  uint8_t bestPointSize = 0;
  uint8_t bestDelta = UINT8_MAX;
  for (const uint8_t logicalSize : availableReaderSizeEnums(style)) {
    const auto* mapped = findClosestReaderSize(logicalSize, style);
    if (!mapped) continue;
    const uint8_t delta =
        mapped->pointSize > targetPointSize ? mapped->pointSize - targetPointSize : targetPointSize - mapped->pointSize;
    if (bestEnum < 0 || delta < bestDelta || (delta == bestDelta && mapped->pointSize < bestPointSize)) {
      bestEnum = logicalSize;
      bestPointSize = mapped->pointSize;
      bestDelta = delta;
    }
  }
  return bestEnum;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableReaderSizeEnums(const uint8_t style) const {
  std::vector<uint8_t> result;
  result.reserve(ReaderFontSize::COUNT);
  std::vector<uint8_t> physicalSizes;
  for (const auto& file : files) {
    if (file.style == style &&
        std::find(physicalSizes.begin(), physicalSizes.end(), file.pointSize) == physicalSizes.end()) {
      physicalSizes.push_back(file.pointSize);
    }
  }
  std::sort(physicalSizes.begin(), physicalSizes.end());

  for (const uint8_t physicalSize : physicalSizes) {
    uint8_t representative = UINT8_MAX;
    uint8_t bestDelta = UINT8_MAX;
    for (uint8_t index = 0; index < ReaderFontSize::COUNT; ++index) {
      const uint8_t target = ReaderFontSize::pointSize(index);
      const auto* selected = findClosestPointSize(target, style);
      if (!selected || selected->pointSize != physicalSize) continue;
      const uint8_t delta = physicalSize > target ? physicalSize - target : target - physicalSize;
      if (delta < bestDelta) {
        representative = index;
        bestDelta = delta;
      }
    }
    if (representative != UINT8_MAX) result.push_back(representative);
  }
  std::sort(result.begin(), result.end());
  return result;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; scanDirectory() defends against accidental
  // (pointSize, style) collisions in that scenario.
  style = 0;
  return true;
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    if (!FontStorageContract::isValidCpfontFilename(nameBuffer)) continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) continue;

    // Reject duplicate (pointSize, style) entries in the same family. With
    // v4's bundle-everything design parseFilename always returns style=0, so
    // two files at the same size in the same family would silently shadow
    // each other in findFile(). Skip the duplicate and warn.
    bool duplicate = false;
    for (const auto& existing : family.files) {
      if (existing.pointSize == size && existing.style == style) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
      continue;
    }

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    if (info.path.size() >= FontStorageContract::FONT_PATH_CAPACITY) {
      LOG_ERR("SDREG", "Font path is too long — skipping %s", nameBuffer);
      continue;
    }
    info.pointSize = size;
    info.style = style;
    family.files.push_back(std::move(info));
  }
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
void SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
      if (!FontStorageContract::isValidFamilyName(nameBuffer)) {
        LOG_ERR("SDREG", "Invalid font family name — skipping");
        continue;
      }

      // De-dup by family name across roots.
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == nameBuffer) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      SdCardFontRegistry::scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
        out.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", out.back().name.c_str(),
                static_cast<int>(out.back().files.size()), rootPath);
      }
    } else {
      entry.close();
    }
  }
}

bool SdCardFontRegistry::discover() {
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  scanRoot(FONTS_DIR_HIDDEN, families_);
  scanRoot(FONTS_DIR_VISIBLE, families_);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!FontStorageContract::isValidFamilyName(familyName)) return nullptr;
  char path[FontStorageContract::FONT_PATH_CAPACITY];
  int written = snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) return nullptr;
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  written = snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) return nullptr;
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
