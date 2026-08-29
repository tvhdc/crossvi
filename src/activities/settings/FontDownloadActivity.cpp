#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <array>
#include <cstring>
#include <limits>

#include "FontStorageUtils.h"
#include "MappedInputManager.h"
#include "ReleaseJsonParser.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/WifiLifecycle.h"

namespace {
constexpr const char* FONT_MANIFEST_TMP = "/fonts_manifest.tmp";
constexpr const char* FONT_RELEASE_TMP = "/fonts_release.tmp";
constexpr size_t MAX_FONT_MANIFEST_BYTES = 256 * 1024;
constexpr size_t MAX_FONT_RELEASE_METADATA_BYTES = 2 * 1024 * 1024;
constexpr size_t RELEASE_READ_CHUNK = 512;

bool extractSha256(const char* digest, std::string& sha256) {
  if (!digest || strlen(digest) != 71 || memcmp(digest, "sha256:", 7) != 0) return false;
  for (size_t i = 7; i < 71; ++i) {
    const char byte = digest[i];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F'))) {
      return false;
    }
  }
  sha256.assign(digest + 7, 64);
  return true;
}

bool parseReleaseMetadata(const ReleaseJsonParser::AssetVisitor visitor, void* context) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", FONT_RELEASE_TMP, file)) {
    LOG_ERR("FONT", "Failed to open font release metadata");
    return false;
  }

  ReleaseJsonParser parser;
  parser.setAssetVisitor(visitor, context);
  std::array<char, RELEASE_READ_CHUNK> buffer;
  while (true) {
    const int read = file.read(buffer.data(), buffer.size());
    if (read <= 0) break;
    parser.feed(buffer.data(), static_cast<size_t>(read));
  }
  const bool readOk = file.getError() == 0;
  const bool closed = file.close();
  const bool parsed = readOk && parser.finish();
  if (!parsed || !closed || !parser.foundTag() || strcmp(parser.getTagName(), FONT_RELEASE_TAG) != 0) {
    LOG_ERR("FONT", "Invalid font release metadata or tag");
    return false;
  }
  return true;
}

struct ManifestDigestCapture {
  std::string sha256;
  bool seen = false;
};

bool captureManifestDigest(void* context, const char* name, const char*, size_t, const char* digest) {
  if (strcmp(name, "fonts.json") != 0) return true;
  auto* capture = static_cast<ManifestDigestCapture*>(context);
  if (capture->seen || !extractSha256(digest, capture->sha256)) return false;
  capture->seen = true;
  return true;
}
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  // TLS certificate parsing needs a large contiguous allocation. A selected
  // SD reader font is unrelated to this screen and must not remain resident.
  sdFontSystem.releaseLoadedFont(renderer);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  Storage.remove(FONT_MANIFEST_TMP);
  Storage.remove(FONT_RELEASE_TMP);

  std::vector<ManifestFamily>().swap(families_);
  std::string().swap(baseUrl_);
  std::string().swap(downloadingFamilyName_);
  std::string().swap(errorMessage_);

  if (!WifiLifecycle::shutDown()) {
    silentRestart();
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  loadManifest();
}

void FontDownloadActivity::loadManifest() {
  retryOperation_ = RetryOperation::MANIFEST;
  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      families_.clear();
      baseUrl_.clear();
      state_ = ERROR;
    }
    // fetchAndParseManifest() runs synchronously after the loading frame. Make
    // the fail-closed result visible immediately instead of leaving the user
    // on a stale "loading" screen after a rejected/failed HTTP request.
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    retryOperation_ = RetryOperation::NONE;
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
  requestUpdate();
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // GitHub's API stays on the low-memory-friendly origin and supplies a
  // SHA-256 for every release asset. Keep the response on SD and stream-parse
  // it so the TLS stack never competes with a large JSON document in RAM.
  if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
  LOG_DBG("FONT", "Release metadata request heap: free=%u maxalloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  auto result = HttpDownloader::downloadToFile(FONT_RELEASE_API_URL, FONT_RELEASE_TMP, nullptr, nullptr, "", "", true,
                                               MAX_FONT_RELEASE_METADATA_BYTES);
  if (result == HttpDownloader::HTTP_ERROR) {
    LOG_DBG("FONT", "Retrying font release metadata after network failure");
    if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
    result = HttpDownloader::downloadToFile(FONT_RELEASE_API_URL, FONT_RELEASE_TMP, nullptr, nullptr, "", "", true,
                                            MAX_FONT_RELEASE_METADATA_BYTES);
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch release metadata from %s (result %d)", FONT_RELEASE_API_URL,
            static_cast<int>(result));
    errorMessage_ = I18N.get(result == HttpDownloader::FILE_ERROR ? StrId::STR_FONT_LIST_STORAGE_ERROR
                                                                  : StrId::STR_FONT_LIST_NETWORK_ERROR);
    Storage.remove(FONT_RELEASE_TMP);
    Storage.remove(FONT_MANIFEST_TMP);
    return false;
  }

  ManifestDigestCapture manifestDigest;
  if (!parseReleaseMetadata(captureManifestDigest, &manifestDigest) || !manifestDigest.seen) {
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    Storage.remove(FONT_RELEASE_TMP);
    Storage.remove(FONT_MANIFEST_TMP);
    return false;
  }

  // The manifest itself is now authenticated before it can select filenames,
  // sizes or checksums. The verified digest permits the GitHub CDN hop to use
  // the downloader's low-memory transport without weakening file integrity.
  LOG_DBG("FONT", "Manifest request heap: free=%u maxalloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  result = HttpDownloader::downloadGithubReleaseAssetToFile(FONT_MANIFEST_URL, manifestDigest.sha256, FONT_MANIFEST_TMP,
                                                            nullptr, nullptr, true, MAX_FONT_MANIFEST_BYTES);
  if (result == HttpDownloader::HTTP_ERROR) {
    LOG_DBG("FONT", "Retrying verified font manifest after network failure");
    if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
    result = HttpDownloader::downloadGithubReleaseAssetToFile(
        FONT_MANIFEST_URL, manifestDigest.sha256, FONT_MANIFEST_TMP, nullptr, nullptr, true, MAX_FONT_MANIFEST_BYTES);
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch verified manifest from %s (result %d)", FONT_MANIFEST_URL,
            static_cast<int>(result));
    if (result == HttpDownloader::FILE_ERROR)
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_STORAGE_ERROR);
    else if (result == HttpDownloader::INTEGRITY_ERROR)
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    else
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_NETWORK_ERROR);
    Storage.remove(FONT_RELEASE_TMP);
    Storage.remove(FONT_MANIFEST_TMP);
    return false;
  }

  if (parseCachedManifest()) return true;
  Storage.remove(FONT_MANIFEST_TMP);
  return false;
}

bool FontDownloadActivity::parseCachedManifest() {
  // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", FONT_MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_STORAGE_ERROR);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();

  if (err) {
    LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    return false;
  }

  int version = doc["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  if (baseUrl_.empty()) {
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    return false;
  }
  families_.clear();
  fontInstaller_.refreshRegistry();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  if (familiesArr.size() > SdCardFontRegistry::MAX_SD_FAMILIES) {
    LOG_ERR("FONT", "Manifest contains too many font families: %zu", familiesArr.size());
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    return false;
  }
  families_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    family.name = fObj["name"] | "";
    family.description = fObj["description"] | "";
    if (!FontInstaller::isValidFamilyName(family.name.c_str())) {
      LOG_ERR("FONT", "Malformed manifest family name");
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
      return false;
    }

    for (JsonVariant s : fObj["styles"].as<JsonArray>()) {
      family.styles.push_back(s.as<std::string>());
    }

    family.totalSize = 0;
    JsonArray fileEntries = fObj["files"].as<JsonArray>();
    if (fileEntries.size() == 0 || fileEntries.size() > SdCardFontRegistry::MAX_FILES_PER_FAMILY) {
      LOG_ERR("FONT", "Invalid file count for family %s: %zu", family.name.c_str(), fileEntries.size());
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
      return false;
    }
    for (JsonObject fileObj : fileEntries) {
      ManifestFile file;
      file.name = fileObj["name"] | "";
      file.sha256 = fileObj["sha256"] | "";
      file.size = fileObj["size"] | 0;

      if (!FontInstaller::isValidCpfontFilename(file.name.c_str()) || file.size == 0 ||
          (!file.sha256.empty() && file.sha256.size() != 64) || !fileObj["crc32"].is<uint32_t>() ||
          family.totalSize > std::numeric_limits<size_t>::max() - file.size) {
        LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", file.name.c_str());
        errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
        return false;
      }
      file.crc32 = fileObj["crc32"].as<uint32_t>();

      family.totalSize += file.size;
      family.files.push_back(std::move(file));
    }

    if (!recoverFamilyTransactions(family)) {
      errorMessage_ = I18N.get(StrId::STR_FONT_LIST_STORAGE_ERROR);
      return false;
    }
    family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());

    // Update checks run only while this user-opened download screen is loading.
    // CRC32 catches rebuilt fonts whose byte length happens to stay unchanged.
    if (family.installed) {
      for (const auto& file : family.files) {
        char path[FontStorageUtils::FONT_PATH_CAPACITY];
        if (!FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path)) ||
            FontStorageUtils::fileMatches(path, file.size, file.crc32) != FontStorageUtils::FileMatch::Match) {
          family.hasUpdate = true;
          break;
        }
      }
    }

    families_.push_back(std::move(family));
  }

  if (!attachReleaseDigests()) {
    errorMessage_ = I18N.get(StrId::STR_FONT_LIST_INVALID);
    return false;
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

bool FontDownloadActivity::attachReleaseDigest(void* context, const char* name, const char*, const size_t size,
                                               const char* digest) {
  auto* activity = static_cast<FontDownloadActivity*>(context);
  for (auto& family : activity->families_) {
    for (auto& file : family.files) {
      if (file.name != name) continue;
      std::string sha256;
      if (file.releaseDigestSeen || size != file.size || !extractSha256(digest, sha256) ||
          (!file.sha256.empty() && file.sha256 != sha256)) {
        return false;
      }
      file.sha256 = std::move(sha256);
      file.releaseDigestSeen = true;
    }
  }
  return true;
}

bool FontDownloadActivity::attachReleaseDigests() {
  for (auto& family : families_) {
    for (auto& file : family.files) file.releaseDigestSeen = false;
  }
  if (!parseReleaseMetadata(attachReleaseDigest, this)) return false;
  for (const auto& family : families_) {
    for (const auto& file : family.files) {
      if (!file.releaseDigestSeen || file.sha256.size() != 64) {
        LOG_ERR("FONT", "Missing verified release digest for %s", file.name.c_str());
        return false;
      }
    }
  }
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  retryOperation_ = RetryOperation::DOWNLOAD_ALL;
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  retryOperation_ = RetryOperation::UPDATE_ALL;
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
}

namespace {
struct FamilyValidationContext {
  FontDownloadActivity* activity;
  const void* family;
};

bool validateInstalledFamily(const char* directory, void* opaque) {
  return static_cast<FontInstaller*>(opaque)->validateFamilyDirectory(directory);
}
}  // namespace

bool FontDownloadActivity::validateFamilyDirectoryCallback(const char* directory, void* opaque) {
  const auto* context = static_cast<FamilyValidationContext*>(opaque);
  return context && context->activity && context->family &&
         context->activity->validateFamilyDirectory(directory, *static_cast<const ManifestFamily*>(context->family));
}

bool FontDownloadActivity::validateFamilyDirectory(const char* directory, const ManifestFamily& family) {
  for (const auto& file : family.files) {
    char path[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!FontStorageUtils::buildFilePath(directory, file.name.c_str(), path, sizeof(path)) ||
        FontStorageUtils::fileMatches(path, file.size, file.crc32) != FontStorageUtils::FileMatch::Match ||
        !fontInstaller_.validateCpfontFile(path)) {
      return false;
    }
  }
  return true;
}

bool FontDownloadActivity::recoverFamilyTransactions(const ManifestFamily& family) {
  FamilyValidationContext context{this, &family};
  bool touched = false;
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  for (const char* root : roots) {
    char finalDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    char stagingDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    char backupDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!FontInstaller::buildFamilyPathAtRoot(root, family.name.c_str(), finalDirectory, sizeof(finalDirectory)) ||
        !FontStorageUtils::buildTransactionDirectoryPath(root, family.name.c_str(), ".download.tmp", stagingDirectory,
                                                         sizeof(stagingDirectory)) ||
        !FontStorageUtils::buildTransactionDirectoryPath(root, family.name.c_str(), ".download.bak", backupDirectory,
                                                         sizeof(backupDirectory))) {
      return false;
    }
    if (!Storage.exists(stagingDirectory) && !Storage.exists(backupDirectory)) continue;
    touched = true;
    if (FontStorageUtils::recoverFamily(finalDirectory, stagingDirectory, backupDirectory,
                                        validateFamilyDirectoryCallback, &context, validateInstalledFamily,
                                        &fontInstaller_) == FontStorageUtils::FamilyTransactionStatus::IoError) {
      return false;
    }
  }
  if (touched) fontInstaller_.refreshRegistry();
  return true;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  const size_t familyStartFileIndex = currentFileIndex_;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    downloadingFamilyName_ = family.name;
    fileProgress_ = 0;
    fileTotal_ = 0;
    lastNotifiedPercent_ = -1;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  const char* root = FontInstaller::rootForFamily(family.name.c_str());
  if (!fontInstaller_.ensureRootDir(root)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  char finalDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
  char stagingDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
  char backupDirectory[FontStorageUtils::FONT_PATH_CAPACITY];
  if (!FontInstaller::buildFamilyPathAtRoot(root, family.name.c_str(), finalDirectory, sizeof(finalDirectory)) ||
      !FontStorageUtils::buildTransactionDirectoryPath(root, family.name.c_str(), ".download.tmp", stagingDirectory,
                                                       sizeof(stagingDirectory)) ||
      !FontStorageUtils::buildTransactionDirectoryPath(root, family.name.c_str(), ".download.bak", backupDirectory,
                                                       sizeof(backupDirectory))) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Font path is too long";
    return;
  }

  FamilyValidationContext validationContext{this, &family};
  if (FontStorageUtils::recoverFamily(finalDirectory, stagingDirectory, backupDirectory,
                                      validateFamilyDirectoryCallback, &validationContext, validateInstalledFamily,
                                      &fontInstaller_) == FontStorageUtils::FamilyTransactionStatus::IoError) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to recover font family";
    return;
  }
  if (!Storage.mkdir(stagingDirectory)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font staging directory";
    return;
  }

  // The manifest contains every filename/style for every family. Keep only
  // the selected family while TLS is active, then rebuild the list from the
  // cached manifest on SD. This avoids holding roughly 20 families of heap
  // allocations during certificate verification on X3.
  ManifestFamily activeFamily = std::move(family);
  std::vector<ManifestFamily>().swap(families_);
  std::string().swap(activeFamily.description);
  std::vector<std::string>().swap(activeFamily.styles);
  std::string().swap(errorMessage_);

  FamilyValidationContext activeValidationContext{this, &activeFamily};
  const auto restoreFamilyList = [this]() {
    if (parseCachedManifest()) return true;
    RenderLock lock(*this);
    families_.clear();
    baseUrl_.clear();
    retryOperation_ = RetryOperation::MANIFEST;
    state_ = ERROR;
    errorMessage_ = "Failed to reload font list";
    return false;
  };
  const auto failDownload = [this, &activeFamily, familyStartFileIndex, stagingDirectory,
                             &restoreFamilyList](const std::string& message) {
    FontStorageUtils::discardStagingFamily(stagingDirectory);
    std::vector<ManifestFile>().swap(activeFamily.files);
    if (!restoreFamilyList()) return;
    RenderLock lock(*this);
    currentFileIndex_ = familyStartFileIndex;
    state_ = ERROR;
    errorMessage_ = message;
  };

  for (size_t i = 0; i < activeFamily.files.size(); i++) {
    const auto& file = activeFamily.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
      lastNotifiedPercent_ = -1;
    }
    requestUpdateAndWait();

    // The loading frame is now visible and no render is active. Release any
    // reader glyph pages before TLS parses GitHub's large redirect headers.
    if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
    LOG_DBG("FONT", "Font request heap: free=%u maxalloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    char stagingPath[FontStorageUtils::FONT_PATH_CAPACITY];
    if (!FontStorageUtils::buildFilePath(stagingDirectory, file.name.c_str(), stagingPath, sizeof(stagingPath))) {
      failDownload("Font path is too long: " + file.name);
      return;
    }

    std::string url = baseUrl_ + file.name;

    const auto downloadFile = [this, &url, &stagingPath, &file]() {
      return HttpDownloader::downloadGithubReleaseAssetToFile(
          url, file.sha256, stagingPath,
          [this](size_t downloaded, size_t total) {
            fileProgress_ = downloaded;
            if (total > 0) fileTotal_ = total;
            mappedInput.update();
            if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
                mappedInput.wasPressed(MappedInputManager::Button::Back)) {
              cancelRequested_ = true;
            }
            // The downloader reports every small network chunk, while an e-ink
            // refresh cannot usefully represent sub-percent changes.
            int percent = fileTotal_ > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100u / fileTotal_) : 0;
            if (percent > 100) percent = 100;
            if (percent != lastNotifiedPercent_) {
              lastNotifiedPercent_ = percent;
              requestUpdate(true);
            }
          },
          &cancelRequested_, false, file.size);
    };

    auto result = downloadFile();
    if (result == HttpDownloader::HTTP_ERROR && !cancelRequested_) {
      LOG_DBG("FONT", "Retrying %s after network failure", file.name.c_str());
      {
        // Progress renders may have repopulated glyph caches during the first
        // attempt. Hold the activity render lock while releasing them again.
        RenderLock lock(*this);
        if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
        fileProgress_ = 0;
        lastNotifiedPercent_ = -1;
      }
      result = downloadFile();
    }

    if (result == HttpDownloader::ABORTED) {
      FontStorageUtils::discardStagingFamily(stagingDirectory);
      std::vector<ManifestFile>().swap(activeFamily.files);
      if (!restoreFamilyList()) return;
      {
        RenderLock lock(*this);
        retryOperation_ = RetryOperation::NONE;
        state_ = FAMILY_LIST;
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      if (result == HttpDownloader::FILE_ERROR)
        failDownload("SD card write failed: " + file.name);
      else if (result == HttpDownloader::INTEGRITY_ERROR)
        failDownload("Font verification failed: " + file.name);
      else
        failDownload("Network error: " + file.name);
      return;
    }

    uint32_t actualCrc = 0;
    uint64_t actualSize = 0;
    if (!FontStorageUtils::computeFileCrc32(stagingPath, actualCrc, actualSize) || actualSize != file.size) {
      LOG_ERR("FONT", "Invalid downloaded size/read for %s: got %llu expected %zu", file.name.c_str(),
              static_cast<unsigned long long>(actualSize), file.size);
      failDownload("Invalid downloaded file: " + file.name);
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      failDownload("Checksum mismatch: " + file.name);
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(stagingPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", stagingPath);
      failDownload("Invalid font file: " + file.name);
      return;
    }
    currentFileIndex_++;
  }

  if (FontStorageUtils::publishFamily(
          finalDirectory, stagingDirectory, backupDirectory, validateFamilyDirectoryCallback, &activeValidationContext,
          validateInstalledFamily, &fontInstaller_) != FontStorageUtils::FamilyTransactionStatus::Published) {
    LOG_ERR("FONT", "Failed to publish font family: %s", activeFamily.name.c_str());
    failDownload("Failed to install font family: " + activeFamily.name);
    return;
  }

  std::vector<ManifestFile>().swap(activeFamily.files);
  if (!restoreFamilyList()) return;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  retryOperation_ = RetryOperation::NONE;
  const int pendingDeleteFamilyIndex = familyIndexFromList(selectedIndex_);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) return;

  deleteSelectedFamily();
}

void FontDownloadActivity::deleteSelectedFamily() {
  const int familyIndex = familyIndexFromList(selectedIndex_);
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) {
    RenderLock lock(*this);
    retryOperation_ = RetryOperation::NONE;
    state_ = FAMILY_LIST;
    return;
  }

  auto& family = families_[familyIndex];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    retryOperation_ = RetryOperation::DELETE_FAMILY;
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    RenderLock lock(*this);
    retryOperation_ = RetryOperation::NONE;
    state_ = FAMILY_LIST;
  }
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  return family.installed && !family.hasUpdate;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        if (isDownloadAllRow(selectedIndex_)) {
          currentFileIndex_ = 0;
          currentFileTotal_ = 0;
          for (const auto& f : families_) {
            if (!f.installed) currentFileTotal_ += f.files.size();
          }

          downloadAll();
        } else if (isUpdateAllRow(selectedIndex_)) {
          currentFileIndex_ = 0;
          currentFileTotal_ = 0;
          for (const auto& f : families_) {
            if (f.hasUpdate) currentFileTotal_ += f.files.size();
          }
          updateAll();
        } else {
          auto& family = families_[familyIndexFromList(selectedIndex_)];
          if (!family.installed || family.hasUpdate) {
            retryOperation_ = RetryOperation::SINGLE_FAMILY;
            currentFileIndex_ = 0;
            currentFileTotal_ = family.files.size();
            downloadFamily(family);
          } else {
            promptDeleteSelectedFamily();
            return;
          }
        }
        requestUpdateAndWait();
        return;
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        retryOperation_ = RetryOperation::NONE;
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        retryOperation_ = RetryOperation::NONE;
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      switch (retryOperation_) {
        case RetryOperation::MANIFEST:
          loadManifest();
          return;
        case RetryOperation::DOWNLOAD_ALL:
          downloadAll();
          break;
        case RetryOperation::UPDATE_ALL:
          updateAll();
          break;
        case RetryOperation::DELETE_FAMILY:
          deleteSelectedFamily();
          break;
        case RetryOperation::SINGLE_FAMILY:
          if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
            downloadFamily(families_[downloadingFamilyIndex_]);
            break;
          }
          [[fallthrough]];
        case RetryOperation::NONE: {
          {
            RenderLock lock(*this);
            state_ = FAMILY_LIST;
          }
          requestUpdate();
          return;
        }
      }
      requestUpdateAndWait();
      return;
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index)) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            }
            if (isUpdateAllRow(index)) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& f = families_[familyIndexFromList(index)];
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + downloadingFamilyName_ + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      const auto lines =
          renderer.wrappedText(UI_10_FONT_ID, errorMessage_.c_str(), pageWidth - metrics.contentSidePadding * 2, 3);
      int errorY = centerY + metrics.verticalSpacing;
      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, errorY, line.c_str());
        errorY += lineHeight;
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
