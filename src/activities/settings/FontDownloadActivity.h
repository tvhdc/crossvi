#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

// Manifest + .cpfont assets are published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#ifndef FONT_RELEASE_TAG
#define FONT_RELEASE_TAG \
  "sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY(FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION)
#endif

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/" FONT_RELEASE_TAG "/fonts.json"
#endif

#ifndef FONT_RELEASE_API_URL
#define FONT_RELEASE_API_URL \
  "https://api.github.com/repos/crosspoint-reader/crosspoint-fonts/releases/tags/" FONT_RELEASE_TAG
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  enum class RetryOperation {
    NONE,
    MANIFEST,
    SINGLE_FAMILY,
    DOWNLOAD_ALL,
    UPDATE_ALL,
    DELETE_FAMILY,
  };

  struct ManifestFile {
    std::string name;
    std::string sha256;
    size_t size = 0;
    uint32_t crc32 = 0;
    bool releaseDigestSeen = false;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    std::vector<std::string> styles;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int lastNotifiedPercent_ = -1;
  int downloadingFamilyIndex_ = 0;
  std::string downloadingFamilyName_;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  RetryOperation retryOperation_ = RetryOperation::NONE;

  void onWifiSelectionComplete(bool success);
  void loadManifest();
  bool fetchAndParseManifest();
  bool parseCachedManifest();
  bool attachReleaseDigests();
  static bool attachReleaseDigest(void* context, const char* name, const char* url, size_t size, const char* digest);
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  void deleteSelectedFamily();
  bool validateFamilyDirectory(const char* directory, const ManifestFamily& family);
  static bool validateFamilyDirectoryCallback(const char* directory, void* context);
  bool recoverFamilyTransactions(const ManifestFamily& family);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
