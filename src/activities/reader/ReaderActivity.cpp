#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Memory.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "Epub/SourceIdentityStore.h"
#include "EpubReaderActivity.h"
#include "PerBookReaderSettingsBridge.h"
#include "PerBookReaderSettingsStore.h"
#include "ReadingStatsCompletionTransaction.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"
#include "util/BookmarkUtil.h"

namespace {

bool isLoadedBookSettings(const PerBookReaderSettingsStore::LoadStatus status) {
  return status == PerBookReaderSettingsStore::LoadStatus::LOADED ||
         status == PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP ||
         status == PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP;
}

void persistMissingBookFontFallback(const std::string& cachePath,
                                    const PerBookReaderSettingsStore::LoadStatus loadStatus,
                                    const bool settingsWritable, PerBookReaderSettings& bookSettings) {
  if (!settingsWritable || !isLoadedBookSettings(loadStatus) || SETTINGS.sdFontFamilyName[0] != '\0' ||
      !applyMissingSdFontFallback(bookSettings, SETTINGS.fontFamily, SETTINGS.fontSize)) {
    return;
  }

  const auto saveStatus = PerBookReaderSettingsStore::save(cachePath, bookSettings);
  if (saveStatus != PerBookReaderSettingsStore::SaveStatus::SAVED) {
    LOG_ERR("READER", "Could not persist missing per-book font fallback (status %u): %s",
            static_cast<unsigned>(saveStatus), cachePath.c_str());
  } else {
    LOG_DBG("READER", "Persisted missing per-book font fallback: %s", cachePath.c_str());
  }
}

}  // namespace

ReaderActivity::~ReaderActivity() = default;

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool ReaderActivity::skipDerivedCoverCacheBuild() const {
  const bool openedFromCoverlessSurface =
      openOrigin == ReaderOpenOrigin::HomeRecent || openOrigin == ReaderOpenOrigin::SavedItems;
  return CrossPointSettings::skipReaderCoverCacheBuild(SETTINGS.homeLayout, SETTINGS.libraryView,
                                                       openedFromCoverlessSurface);
}

int ReaderActivity::initialRefreshCountdown() const {
  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

void ReaderActivity::validateInitialBookmarkJump(const BookmarkEntry::PositionKind kind) {
  if (!initialBookmarkJump) return;
  if (!initialBookmarkJump->hasBookmarkFingerprint) {
    LOG_ERR("READER", "Rejected bookmark jump without an exact bookmark identity: %s", initialBookPath.c_str());
    initialBookmarkJump.reset();
    return;
  }

  const std::string canonical = BookmarkUtil::getBookmarkPath(initialBookPath);
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(initialBookPath);
  const std::string path = BookmarkUtil::canonicalFamilyExists(initialBookPath) ? canonical : legacy;
  std::vector<BookmarkEntry> bookmarks;
  BookmarkBookMetadata metadata;
  const auto status = JsonSettingsIO::loadBookmarksFromFile(bookmarks, path.c_str(), &metadata);
  const bool valid = status == JsonSettingsIO::BookmarkLoadStatus::Loaded &&
                     BookmarkUtil::metadataMatchesBook(metadata, initialBookPath, kind) &&
                     std::any_of(bookmarks.begin(), bookmarks.end(), [&](const BookmarkEntry& bookmark) {
                       return bookmark.positionKind == kind &&
                              BookmarkUtil::fingerprint(bookmark) == initialBookmarkJump->bookmarkFingerprint;
                     });
  if (!valid) {
    // loadEpub/loadTxt/loadXtc has already validated source identity and
    // quarantined state for a same-path replacement. Only the bookmark that
    // survived that boundary may now influence the reader position.
    LOG_ERR("READER", "Rejected stale bookmark after source validation: %s", initialBookPath.c_str());
    initialBookmarkJump.reset();
  }
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path, PerBookReaderSettings& globalSettings,
                                               PerBookReaderSettings& bookSettings, bool& settingsWritable,
                                               bool& deferCoverPreparation,
                                               const ZipFile::SourceIdentity& verifiedEpubIdentity,
                                               BookMetadataCache::LoadStepResult& cacheStepResult,
                                               std::unique_ptr<Epub> preparedEpub) {
  deferCoverPreparation = false;
  const auto createEpub = [&]() { return makeUniqueNoThrow<Epub>(path, "/.crosspoint", verifiedEpubIdentity); };
  auto epub = std::move(preparedEpub);
  if (!epub || epub->getPath() != path || !epub->prepareForReaderLoadAfterRecovery(verifiedEpubIdentity)) {
    epub = createEpub();
  } else {
    LOG_DBG("READER", "Reusing prepared EPUB source: %s", path.c_str());
  }
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }

  globalSettings = captureReaderSettings();
  bookSettings = globalSettings;
  // An interrupted cache clear may have already moved the settings/statistics
  // files into a sibling staging directory. Recover them before any loader can
  // create replacement state; if recovery is ambiguous, fail closed.
  if (!recoverBookCacheUserState(epub->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged per-book state: %s", epub->getCachePath().c_str());
    return nullptr;
  }

  uint32_t stageStartedMs = static_cast<uint32_t>(millis());
  Epub::SourceBindingStatus bindingStatus = epub->inspectSourceBindingForLoad();
  if (bindingStatus == Epub::SourceBindingStatus::Mismatch) {
    const std::string staleCachePath = epub->getCachePath();
    // Quarantine all old path-keyed state before the new EPUB can inherit it.
    // A partial cleanup is a hard failure: the old identity remains the proof
    // needed to retry safely on a later open.
    if (!resetBookUserStateAfterReplacement(path) || Storage.exists(staleCachePath.c_str())) {
      LOG_ERR("READER", "Could not quarantine stale state for replaced EPUB: %s", path.c_str());
      return nullptr;
    }
    epub = createEpub();
    if (!epub) return nullptr;
    bindingStatus = Epub::SourceBindingStatus::Missing;
  } else if (bindingStatus == Epub::SourceBindingStatus::NewerVersion ||
             bindingStatus == Epub::SourceBindingStatus::Invalid ||
             bindingStatus == Epub::SourceBindingStatus::IoError) {
    LOG_ERR("READER", "EPUB source identity cannot be handled safely (status %u)",
            static_cast<unsigned>(bindingStatus));
    return nullptr;
  }

  if (bindingStatus == Epub::SourceBindingStatus::Missing) {
    // One-time migration for books whose cache/user state predates source
    // identities (including a cache cleared on older firmware). A replacement
    // before this first adoption is fundamentally unknowable.
    LOG_DBG("READER", "Adopting source identity for legacy EPUB state: %s", path.c_str());
  }
  if (bindingStatus != Epub::SourceBindingStatus::Match) {
    if (!epub->bindCurrentSource() || epub->inspectSourceBindingForLoad() != Epub::SourceBindingStatus::Match) {
      LOG_ERR("READER", "Could not persist EPUB source identity: %s", path.c_str());
      return nullptr;
    }
  }
  activityManager.reportReaderOpenStage("epub", "source_binding", stageStartedMs);

  // CrossInk's legacy file remains authoritative and immutable. Migration is
  // best-effort after source identity is proven; any failure only falls back
  // to CrossVi/global settings and must never prevent the book from opening.
  const auto migrationStatus = PerBookReaderSettingsStore::migrateCrossInk(epub->getCachePath(), globalSettings);
  switch (migrationStatus) {
    case PerBookReaderSettingsStore::MigrationStatus::MIGRATED:
      LOG_DBG("READER", "Migrated CrossInk reader settings: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::NEWER_CROSSINK_VERSION:
      LOG_ERR("READER", "Preserving unsupported newer CrossInk reader settings: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::INVALID_LEGACY_FILE:
      LOG_ERR("READER", "Preserving invalid CrossInk reader settings without migration: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::BACKUP_CONFLICT:
      LOG_ERR("READER", "CrossInk reader settings backup conflicts; preserving both files: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::IO_ERROR:
    case PerBookReaderSettingsStore::MigrationStatus::SAVE_FAILED:
      LOG_ERR("READER", "Could not safely migrate CrossInk reader settings (status %u): %s",
              static_cast<unsigned>(migrationStatus), path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::INVALID_DEFAULTS:
      LOG_ERR("READER", "Current reader defaults are invalid; CrossInk settings were preserved: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::NO_LEGACY_FILE:
    case PerBookReaderSettingsStore::MigrationStatus::CROSSVI_FILE_PRESENT:
      break;
  }

  openingEpubCacheStartedMs = static_cast<uint32_t>(millis());
  cacheStepResult = epub->beginCacheInspection();

  const auto settingsStatus = PerBookReaderSettingsStore::load(epub->getCachePath(), bookSettings);
  settingsWritable = settingsStatus != PerBookReaderSettingsStore::LoadStatus::NEWER_VERSION &&
                     settingsStatus != PerBookReaderSettingsStore::LoadStatus::IO_ERROR;
  if (settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED ||
      settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP ||
      settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP) {
    applyEffectiveBookReaderSettings(globalSettings, bookSettings);
  } else {
    bookSettings = globalSettings;
    applyEffectiveBookReaderSettings(globalSettings, bookSettings);
  }
  // A per-book SD font must be active before layout starts. Invalid book-only
  // choices are cleared in memory without leaking the override to settings.json.
  sdFontSystem.ensureLoaded(renderer, false);
  persistMissingBookFontFallback(epub->getCachePath(), settingsStatus, settingsWritable, bookSettings);
  const bool skipCoverCacheBuild = skipDerivedCoverCacheBuild();
  const bool needsShared =
      !skipCoverCacheBuild && CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel =
      !skipCoverCacheBuild && CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  deferCoverPreparation = needsShared || needsCarousel;
  return epub;
}

bool ReaderActivity::beginEpubLoad(const std::string& path) {
  openingEpubIdentityJob.reset();
  openingEpubIdentityStartedMs = 0;
  openingEpubFinalIdentityCheck = false;
  std::optional<RawSourceIdentityHandoff> preparedSourceIdentity;
  if (openingPreparedEpub && openingPreparedEpub->getPath() == path) {
    RawSourceIdentityHandoff preparedIdentity;
    if (openingPreparedEpub->getSourceIdentityHandoff(preparedIdentity)) {
      preparedSourceIdentity = std::move(preparedIdentity);
    }
  }
  if (!preparedSourceIdentity) preparedSourceIdentity = std::move(openingPreparedSourceIdentity);
  const bool matchingPreparedIdentity = preparedSourceIdentity && preparedSourceIdentity->valid &&
                                        !preparedSourceIdentity->identity.isRawFile() &&
                                        preparedSourceIdentity->path == path;
  const bool replacementArtifactWasPresent = matchingPreparedIdentity && hasBookFileReplacementArtifacts(path);
  std::optional<ZipFile::SourceIdentity> recoveredIdentity;
  const uint32_t recoveryStartedMs = static_cast<uint32_t>(millis());
  if (!recoverInterruptedBookFileReplacement(path, &recoveredIdentity, completionStatsWritableAtOpen)) {
    LOG_ERR("READER", "Could not recover interrupted EPUB replacement: %s", path.c_str());
    return false;
  }
  activityManager.reportReaderOpenStage("epub", "replacement_recovery", recoveryStartedMs);
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return false;
  }
  if (recoveredIdentity) {
    openingPreparedEpub.reset();
    return finishEpubLoad(*recoveredIdentity);
  }

  if (matchingPreparedIdentity && !replacementArtifactWasPresent && !hasBookFileReplacementArtifacts(path) &&
      Storage.probeMedia()) {
    HalFile preparedFile;
    const bool sameDirectoryEntry = Storage.openFileForRead("READER", path, preparedFile) &&
                                    preparedSourceIdentity->matchesOpenFile(path, preparedFile);
    preparedFile.close();
    if (sameDirectoryEntry) return finishEpubLoad(preparedSourceIdentity->identity);
  }

  openingPreparedEpub.reset();
  openingEpubIdentityJob = makeUniqueNoThrow<ZipSourceIdentityJob>();
  openingEpubIdentityStartedMs = static_cast<uint32_t>(millis());
  if (!openingEpubIdentityJob || !openingEpubIdentityJob->begin(path)) {
    openingEpubIdentityJob.reset();
    LOG_ERR("READER", "Could not begin EPUB source fingerprint: %s", path.c_str());
    return false;
  }
  return true;
}

bool ReaderActivity::finishEpubLoad(const ZipFile::SourceIdentity& verifiedEpubIdentity) {
  openingSettingsWritable = true;
  openingEpubDeferCoverPreparation = false;
  BookMetadataCache::LoadStepResult cacheStepResult = BookMetadataCache::LoadStepResult::Error;
  const uint32_t prepareStartedMs = static_cast<uint32_t>(millis());
  auto epub =
      loadEpub(initialBookPath, openingGlobalSettings, openingBookSettings, openingSettingsWritable,
               openingEpubDeferCoverPreparation, verifiedEpubIdentity, cacheStepResult, std::move(openingPreparedEpub));
  activityManager.reportReaderOpenStage("epub", "reader_prepare", prepareStartedMs);
  if (!epub) return false;

  openingEpub = std::move(epub);
  openingEpubExpectedIdentity = verifiedEpubIdentity;
  openingEpubCacheInspection = cacheStepResult == BookMetadataCache::LoadStepResult::InProgress;
  if (openingEpubCacheInspection) return true;
  if (finishEpubCacheInspection(cacheStepResult)) return true;

  openingEpub.reset();
  applyReaderSettings(openingGlobalSettings);
  sdFontSystem.releaseLoadedFont(renderer);
  return false;
}

bool ReaderActivity::finishEpubCacheInspection(const BookMetadataCache::LoadStepResult result) {
  openingEpubCacheInspection = false;
  activityManager.reportReaderOpenStage("epub", "book_cache_inspect", openingEpubCacheStartedMs);
  openingEpubCacheStartedMs = 0;
  if (!openingEpub) return false;

  const BookMetadataCache::LoadStatus cacheStatus = openingEpub->getCacheLoadStatus();
  if (result == BookMetadataCache::LoadStepResult::Error &&
      (cacheStatus == BookMetadataCache::LoadStatus::NewerVersion ||
       cacheStatus == BookMetadataCache::LoadStatus::IoError)) {
    LOG_ERR("READER", "EPUB cache cannot be handled safely (status %u)", static_cast<unsigned>(cacheStatus));
    return false;
  }

  if (result == BookMetadataCache::LoadStepResult::Loaded) {
    const uint32_t metadataStartedMs = static_cast<uint32_t>(millis());
    const bool loaded =
        openingEpub->loadForCooperativeSourceCheck(true, SETTINGS.epubSafeMode != 0 || SETTINGS.embeddedStyle == 0);
    activityManager.reportReaderOpenStage("epub", "metadata_load", metadataStartedMs);
    if (!loaded) {
      LOG_ERR("READER", "Failed to load cached EPUB metadata");
      return false;
    }

    openingEpubIdentityJob = makeUniqueNoThrow<ZipSourceIdentityJob>();
    openingEpubIdentityStartedMs = static_cast<uint32_t>(millis());
    if (!openingEpubIdentityJob || !openingEpubIdentityJob->begin(initialBookPath)) {
      openingEpubIdentityJob.reset();
      return false;
    }
    openingEpubFinalIdentityCheck = true;
    return true;
  }

  // Missing, legacy, stale, or malformed derived metadata is rebuilt. Replace
  // the preceding popup because it may be narrower and leave stale e-ink text.
  allowFastInitialRefresh = false;
  renderer.clearScreen();
  GUI.drawPopup(renderer, I18N.get(StrId::STR_INDEXING));
  // Keep the indexing scratch loan alive across main-loop steps. The popup
  // remains on the panel and the framebuffer is restored before a transition.
  openingEpubFrameBufferLoan.emplace(renderer);
  openingEpubIndexStartedMs = static_cast<uint32_t>(millis());
  if (openingEpub->beginIndexing(SETTINGS.epubSafeMode != 0 || SETTINGS.embeddedStyle == 0)) return true;

  activityManager.reportReaderOpenStage("epub", "cold_index", openingEpubIndexStartedMs);
  openingEpubIndexStartedMs = 0;
  openingEpubFrameBufferLoan.reset();
  LOG_ERR("READER", "Failed to begin EPUB indexing");
  return false;
}

bool ReaderActivity::beginXtcLoad(const std::string& path) {
  openingXtc.reset();
  openingXtcStartedMs = static_cast<uint32_t>(millis());
  std::unique_ptr<Xtc> preparedXtc = std::move(openingPreparedXtc);
  std::optional<RawSourceIdentityHandoff> preparedSourceIdentity;
  if (preparedXtc && preparedXtc->getPath() == path) {
    RawSourceIdentityHandoff preparedIdentity;
    if (preparedXtc->getSourceIdentityHandoff(preparedIdentity)) {
      preparedSourceIdentity = std::move(preparedIdentity);
    }
  }
  if (!preparedSourceIdentity) preparedSourceIdentity = std::move(openingPreparedSourceIdentity);
  const bool matchingPreparedIdentity = preparedSourceIdentity && preparedSourceIdentity->path == path;
  const bool replacementArtifactWasPresent = matchingPreparedIdentity && hasBookFileReplacementArtifacts(path);
  if (!recoverInterruptedBookFileReplacement(path, nullptr, completionStatsWritableAtOpen)) {
    LOG_ERR("READER", "Could not recover interrupted XTC replacement: %s", path.c_str());
    return false;
  }
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return false;
  }

  if (preparedXtc && preparedXtc->getPath() == path && !recoverBookCacheUserState(preparedXtc->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged XTC state: %s", preparedXtc->getCachePath().c_str());
    return false;
  }
  if (preparedXtc && matchingPreparedIdentity && !replacementArtifactWasPresent &&
      !hasBookFileReplacementArtifacts(path) && Storage.probeMedia()) {
    HalFile preparedFile;
    const bool sameDirectoryEntry = Storage.openFileForRead("READER", path, preparedFile) &&
                                    preparedSourceIdentity->matchesOpenFile(path, preparedFile);
    preparedFile.close();
    if (sameDirectoryEntry) {
      LOG_DBG("READER", "Reusing prepared XTC source: %s", path.c_str());
      openingXtc = std::move(preparedXtc);
      return true;
    }
  }

  openingXtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!openingXtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return false;
  }
  if (!recoverBookCacheUserState(openingXtc->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged XTC state: %s", openingXtc->getCachePath().c_str());
    openingXtc.reset();
    return false;
  }
  const RawSourceIdentityHandoff* reusableIdentity = nullptr;
  if (matchingPreparedIdentity && !replacementArtifactWasPresent && !hasBookFileReplacementArtifacts(path) &&
      Storage.probeMedia()) {
    reusableIdentity = &*preparedSourceIdentity;
  }
  if (!openingXtc->beginLoad(reusableIdentity)) {
    LOG_ERR("READER", "Failed to begin XTC load");
    openingXtc.reset();
    return false;
  }
  return true;
}

bool ReaderActivity::finishXtcLoad(bool& deferCoverPreparation) {
  deferCoverPreparation = false;
  if (!openingXtc || !openingXtc->isLoaded()) return false;
  ZipFile::SourceIdentity currentIdentity;
  if (!openingXtc->getSourceIdentity(currentIdentity)) {
    LOG_ERR("READER", "Could not identify XTC source: %s", initialBookPath.c_str());
    return false;
  }

  ZipFile::SourceIdentity storedIdentity;
  const SourceIdentityStore::LoadStatus identityStatus =
      SourceIdentityStore::load(openingXtc->getCachePath(), storedIdentity);
  const bool identityAlreadyPrimary =
      identityStatus == SourceIdentityStore::LoadStatus::Primary && storedIdentity == currentIdentity;
  switch (identityStatus) {
    case SourceIdentityStore::LoadStatus::Primary:
    case SourceIdentityStore::LoadStatus::Backup:
    case SourceIdentityStore::LoadStatus::Temp:
      if (storedIdentity != currentIdentity) {
        const std::string staleCachePath = openingXtc->getCachePath();
        if (!resetBookUserStateAfterReplacement(initialBookPath) || Storage.exists(staleCachePath.c_str())) {
          LOG_ERR("READER", "Could not quarantine stale XTC state: %s", initialBookPath.c_str());
          return false;
        }
      }
      break;
    case SourceIdentityStore::LoadStatus::Missing:
      LOG_DBG("READER", "Adopting source identity for legacy XTC state: %s", initialBookPath.c_str());
      break;
    case SourceIdentityStore::LoadStatus::NewerVersion:
    case SourceIdentityStore::LoadStatus::Invalid:
    case SourceIdentityStore::LoadStatus::IoError:
      LOG_ERR("READER", "XTC source identity cannot be handled safely (status %u)",
              static_cast<unsigned>(identityStatus));
      return false;
  }

  if (!identityAlreadyPrimary) {
    const SourceIdentityStore::SaveStatus saved =
        SourceIdentityStore::save(openingXtc->getCachePath(), currentIdentity);
    if (saved != SourceIdentityStore::SaveStatus::Saved && saved != SourceIdentityStore::SaveStatus::Unchanged) {
      LOG_ERR("READER", "Could not persist XTC source identity: %s", initialBookPath.c_str());
      return false;
    }
    ZipFile::SourceIdentity verifiedIdentity;
    if (SourceIdentityStore::load(openingXtc->getCachePath(), verifiedIdentity) !=
            SourceIdentityStore::LoadStatus::Primary ||
        verifiedIdentity != currentIdentity) {
      LOG_ERR("READER", "Could not verify XTC source identity: %s", initialBookPath.c_str());
      return false;
    }
  }

  const bool skipCoverCacheBuild = skipDerivedCoverCacheBuild();
  const bool needsShared =
      !skipCoverCacheBuild && CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView);
  const bool needsCarousel =
      !skipCoverCacheBuild && CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout);
  deferCoverPreparation = needsShared || needsCarousel;
  return true;
}

bool ReaderActivity::beginTxtLoad(const std::string& path) {
  openingTxt.reset();
  openingTxtStartedMs = static_cast<uint32_t>(millis());
  std::unique_ptr<Txt> preparedTxt = std::move(openingPreparedTxt);
  std::optional<RawSourceIdentityHandoff> preparedSourceIdentity;
  if (preparedTxt && preparedTxt->getPath() == path) {
    RawSourceIdentityHandoff preparedIdentity;
    if (preparedTxt->getSourceIdentityHandoff(preparedIdentity)) {
      preparedSourceIdentity = std::move(preparedIdentity);
    }
  }
  if (!preparedSourceIdentity) preparedSourceIdentity = std::move(openingPreparedSourceIdentity);
  const bool matchingPreparedIdentity = preparedSourceIdentity && preparedSourceIdentity->path == path;
  const bool replacementArtifactWasPresent = matchingPreparedIdentity && hasBookFileReplacementArtifacts(path);
  if (!recoverInterruptedBookFileReplacement(path, nullptr, completionStatsWritableAtOpen)) {
    LOG_ERR("READER", "Could not recover interrupted text replacement: %s", path.c_str());
    return false;
  }
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return false;
  }

  if (preparedTxt && matchingPreparedIdentity && !replacementArtifactWasPresent &&
      !hasBookFileReplacementArtifacts(path) && Storage.probeMedia()) {
    HalFile preparedFile;
    const bool sameDirectoryEntry = Storage.openFileForRead("READER", path, preparedFile) &&
                                    preparedSourceIdentity->matchesOpenFile(path, preparedFile);
    preparedFile.close();
    if (sameDirectoryEntry) {
      LOG_DBG("READER", "Reusing prepared TXT source: %s", path.c_str());
      openingTxt = std::move(preparedTxt);
      openingGlobalSettings = captureReaderSettings();
      openingBookSettings = openingGlobalSettings;
      if (!recoverBookCacheUserState(openingTxt->getCachePath(), path)) {
        LOG_ERR("READER", "Could not recover staged TXT state: %s", openingTxt->getCachePath().c_str());
        openingTxt.reset();
        return false;
      }
      return true;
    }
  }

  openingTxt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!openingTxt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return false;
  }
  openingGlobalSettings = captureReaderSettings();
  openingBookSettings = openingGlobalSettings;
  if (!recoverBookCacheUserState(openingTxt->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged TXT state: %s", openingTxt->getCachePath().c_str());
    openingTxt.reset();
    return false;
  }
  const RawSourceIdentityHandoff* reusableIdentity = nullptr;
  if (matchingPreparedIdentity && !replacementArtifactWasPresent && !hasBookFileReplacementArtifacts(path) &&
      Storage.probeMedia()) {
    reusableIdentity = &*preparedSourceIdentity;
  }
  if (!openingTxt->beginLoad(reusableIdentity)) {
    LOG_ERR("READER", "Failed to begin TXT load");
    openingTxt.reset();
    return false;
  }
  return true;
}

bool ReaderActivity::finishTxtLoad() {
  if (!openingTxt) return false;
  ZipFile::SourceIdentity currentIdentity;
  if (!openingTxt->getSourceIdentity(currentIdentity)) {
    LOG_ERR("READER", "Could not identify TXT source: %s", initialBookPath.c_str());
    return false;
  }

  ZipFile::SourceIdentity storedIdentity;
  SourceIdentityStore::LoadStatus identityStatus =
      SourceIdentityStore::load(openingTxt->getCachePath(), storedIdentity);
  const bool identityAlreadyPrimary =
      identityStatus == SourceIdentityStore::LoadStatus::Primary && storedIdentity == currentIdentity;
  switch (identityStatus) {
    case SourceIdentityStore::LoadStatus::Primary:
    case SourceIdentityStore::LoadStatus::Backup:
    case SourceIdentityStore::LoadStatus::Temp:
      if (storedIdentity != currentIdentity) {
        const std::string staleCachePath = openingTxt->getCachePath();
        // The path now contains different bytes. Quarantine every path-keyed
        // state file before the replacement can inherit progress/statistics.
        if (!resetBookUserStateAfterReplacement(initialBookPath) || Storage.exists(staleCachePath.c_str())) {
          LOG_ERR("READER", "Could not quarantine stale TXT state: %s", initialBookPath.c_str());
          return false;
        }
      }
      break;
    case SourceIdentityStore::LoadStatus::Missing:
      // One-time adoption for caches created before TXT source bindings. A
      // replacement made before this first upgraded open is unknowable.
      LOG_DBG("READER", "Adopting source identity for legacy TXT state: %s", initialBookPath.c_str());
      break;
    case SourceIdentityStore::LoadStatus::NewerVersion:
    case SourceIdentityStore::LoadStatus::Invalid:
    case SourceIdentityStore::LoadStatus::IoError:
      LOG_ERR("READER", "TXT source identity cannot be handled safely (status %u)",
              static_cast<unsigned>(identityStatus));
      return false;
  }

  if (!identityAlreadyPrimary) {
    const SourceIdentityStore::SaveStatus saved =
        SourceIdentityStore::save(openingTxt->getCachePath(), currentIdentity);
    if (saved != SourceIdentityStore::SaveStatus::Saved && saved != SourceIdentityStore::SaveStatus::Unchanged) {
      LOG_ERR("READER", "Could not persist TXT source identity: %s", initialBookPath.c_str());
      return false;
    }
    ZipFile::SourceIdentity verifiedIdentity;
    if (SourceIdentityStore::load(openingTxt->getCachePath(), verifiedIdentity) !=
            SourceIdentityStore::LoadStatus::Primary ||
        verifiedIdentity != currentIdentity) {
      LOG_ERR("READER", "Could not verify TXT source identity: %s", initialBookPath.c_str());
      return false;
    }
  }

  const PerBookReaderSettingsStore::LoadStatus settingsStatus =
      PerBookReaderSettingsStore::load(openingTxt->getCachePath(), openingBookSettings);
  openingSettingsWritable = settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED ||
                            settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP ||
                            settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP ||
                            settingsStatus == PerBookReaderSettingsStore::LoadStatus::MISSING;
  if (settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED &&
      settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP &&
      settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP) {
    openingBookSettings = openingGlobalSettings;
  }
  applyReaderSettings(openingBookSettings.hasReaderOverrides ? openingBookSettings : openingGlobalSettings);
  sdFontSystem.ensureLoaded(renderer, false);
  persistMissingBookFontFallback(openingTxt->getCachePath(), settingsStatus, openingSettingsWritable,
                                 openingBookSettings);
  return true;
}

void ReaderActivity::cancelCooperativeOpen() {
  if (openingXtc) openingXtc->cancelLoad();
  if (openingTxt) openingTxt->cancelLoad();
  if (openingEpub) {
    openingEpub->cancelCacheInspection();
    openingEpub->cancelIndexing();
  }
  if (openingEpubIdentityJob) openingEpubIdentityJob->cancel();
  openingXtc.reset();
  openingTxt.reset();
  openingEpub.reset();
  openingPreparedXtc.reset();
  openingPreparedTxt.reset();
  openingPreparedEpub.reset();
  openingEpubIdentityJob.reset();
  openingEpubCacheInspection = false;
  openingEpubCacheStartedMs = 0;
  openingEpubIndexStartedMs = 0;
  openingEpubFinalIdentityCheck = false;
  openingXtcStartedMs = 0;
  openingTxtStartedMs = 0;
  openingEpubFrameBufferLoan.reset();
}

void ReaderActivity::pumpCooperativeOpen() {
  constexpr size_t RECORDS_PER_STEP = 4;
  constexpr size_t CACHE_ENTRIES_PER_STEP = 8;
  constexpr size_t FINGERPRINT_BYTES_PER_STEP = 16U * 1024U;

  if (!openingXtc && !openingTxt && !openingEpubIdentityJob && !openingEpub) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    const bool restoreEpubSettings = openingEpub != nullptr;
    cancelCooperativeOpen();
    if (restoreEpubSettings) {
      applyReaderSettings(openingGlobalSettings);
      sdFontSystem.releaseLoadedFont(renderer);
    }
    onGoBack();
    return;
  }

  if (openingEpubIdentityJob) {
    ZipFile::SourceIdentity identity;
    const ZipSourceIdentityJob::StepStatus result = openingEpubIdentityJob->step(FINGERPRINT_BYTES_PER_STEP, identity);
    if (result == ZipSourceIdentityJob::StepStatus::InProgress) return;
    openingEpubIdentityJob.reset();
    const bool finalIdentityCheck = openingEpubFinalIdentityCheck;
    activityManager.reportReaderOpenStage("epub", finalIdentityCheck ? "source_identity_final" : "source_identity",
                                          openingEpubIdentityStartedMs);
    openingEpubIdentityStartedMs = 0;
    if (finalIdentityCheck) {
      openingEpubFinalIdentityCheck = false;
      if (result != ZipSourceIdentityJob::StepStatus::Done || identity != openingEpubExpectedIdentity || !openingEpub) {
        openingEpub.reset();
        applyReaderSettings(openingGlobalSettings);
        sdFontSystem.releaseLoadedFont(renderer);
        onGoBack();
        return;
      }
      validateInitialBookmarkJump(BookmarkEntry::PositionKind::Epub);
      onGoToEpubReader(std::move(openingEpub), std::move(openingGlobalSettings), std::move(openingBookSettings),
                       openingSettingsWritable, openingEpubDeferCoverPreparation);
      return;
    }
    if (result == ZipSourceIdentityJob::StepStatus::Error || !finishEpubLoad(identity)) {
      onGoBack();
    }
    return;
  }

  if (openingEpub) {
    if (openingEpubCacheInspection) {
      const BookMetadataCache::LoadStepResult result = openingEpub->stepCacheInspection(CACHE_ENTRIES_PER_STEP);
      if (result == BookMetadataCache::LoadStepResult::InProgress) return;
      if (!finishEpubCacheInspection(result)) {
        openingEpub.reset();
        applyReaderSettings(openingGlobalSettings);
        sdFontSystem.releaseLoadedFont(renderer);
        onGoBack();
      }
      return;
    }

    const Epub::IndexStepResult result = openingEpub->stepIndexing();
    if (result == Epub::IndexStepResult::InProgress) return;
    activityManager.reportReaderOpenStage("epub", "cold_index", openingEpubIndexStartedMs);
    openingEpubIndexStartedMs = 0;

    // No drawing or activity transition is safe while the framebuffer backs
    // indexing scratch memory.
    openingEpubFrameBufferLoan.reset();
    if (result == Epub::IndexStepResult::Error) {
      openingEpub.reset();
      applyReaderSettings(openingGlobalSettings);
      sdFontSystem.releaseLoadedFont(renderer);
      onGoBack();
      return;
    }

    validateInitialBookmarkJump(BookmarkEntry::PositionKind::Epub);
    onGoToEpubReader(std::move(openingEpub), std::move(openingGlobalSettings), std::move(openingBookSettings),
                     openingSettingsWritable, openingEpubDeferCoverPreparation);
    return;
  }

  if (openingXtc) {
    const Xtc::LoadStepResult result = openingXtc->stepLoad(RECORDS_PER_STEP, FINGERPRINT_BYTES_PER_STEP);
    if (result == Xtc::LoadStepResult::InProgress) return;
    if (result == Xtc::LoadStepResult::Error) {
      activityManager.reportReaderOpenStage("xtc", "source_metadata", openingXtcStartedMs);
      openingXtcStartedMs = 0;
      openingXtc.reset();
      onGoBack();
      return;
    }

    bool deferCoverPreparation = false;
    const bool loaded = finishXtcLoad(deferCoverPreparation);
    activityManager.reportReaderOpenStage("xtc", "source_metadata", openingXtcStartedMs);
    openingXtcStartedMs = 0;
    if (!loaded) {
      openingXtc.reset();
      onGoBack();
      return;
    }
    validateInitialBookmarkJump(BookmarkEntry::PositionKind::FixedLayout);
    onGoToXtcReader(std::move(openingXtc), deferCoverPreparation);
    return;
  }

  if (openingTxt) {
    const Txt::LoadStepResult result = openingTxt->stepLoad(FINGERPRINT_BYTES_PER_STEP);
    if (result == Txt::LoadStepResult::InProgress) return;
    const bool loaded = result != Txt::LoadStepResult::Error && finishTxtLoad();
    activityManager.reportReaderOpenStage("text", "source_settings", openingTxtStartedMs);
    openingTxtStartedMs = 0;
    if (!loaded) {
      openingTxt.reset();
      onGoBack();
      return;
    }
    validateInitialBookmarkJump(BookmarkEntry::PositionKind::Text);
    onGoToTxtReader(std::move(openingTxt), std::move(openingGlobalSettings), std::move(openingBookSettings),
                    openingSettingsWritable);
  }
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub, PerBookReaderSettings globalSettings,
                                      PerBookReaderSettings bookSettings, const bool settingsWritable,
                                      const bool deferCoverPreparation) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  std::optional<ProgressChangeResult> bookmarkJump;
  if (initialBookmarkJump) bookmarkJump = std::move(initialBookmarkJump->progress);
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(
      renderer, mappedInput, std::move(epub), completionStatsWritableAtOpen, std::move(globalSettings),
      std::move(bookSettings), settingsWritable, std::move(initialClippingJump), std::move(bookmarkJump),
      initialRefreshCountdown(), deferCoverPreparation, allowFastInitialRefresh));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(
      std::make_unique<BmpViewerActivity>(renderer, mappedInput, path, readerOpenFeedbackAlreadyShown));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc, const bool deferCoverPreparation) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  std::optional<uint32_t> bookmarkPage;
  if (initialBookmarkJump && initialBookmarkJump->hasFixedPage) bookmarkPage = initialBookmarkJump->page;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(
      renderer, mappedInput, std::move(xtc), completionStatsWritableAtOpen, bookmarkPage, initialRefreshCountdown(),
      deferCoverPreparation, allowFastInitialRefresh));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt, PerBookReaderSettings globalSettings,
                                     PerBookReaderSettings bookSettings, const bool settingsWritable) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  std::optional<ProgressChangeResult> bookmarkJump;
  if (initialBookmarkJump) bookmarkJump = std::move(initialBookmarkJump->progress);
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(
      renderer, mappedInput, std::move(txt), completionStatsWritableAtOpen, std::move(globalSettings),
      std::move(bookSettings), settingsWritable, std::move(initialClippingJump), std::move(bookmarkJump),
      initialRefreshCountdown(), allowFastInitialRefresh));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Direct resume can enter Reader before Home. A normal Home open reuses the
  // successful recovery performed moments earlier instead of rereading both
  // marker candidates.
  if (!completionStatsAlreadyRecovered) {
    const ReadingStatsCompletionTransaction::RecoveryResult completionRecovery =
        ReadingStatsCompletionTransaction::recoverPending();
    completionStatsWritableAtOpen = completionRecovery != ReadingStatsCompletionTransaction::RecoveryResult::Blocked;
  }
  completionStatsAlreadyRecovered = false;
  if (!completionStatsWritableAtOpen) {
    LOG_ERR("READER", "Pending reading-statistics transaction remains blocked");
  }

  if (initialBookPath.empty()) {
    activityManager.cancelReaderOpenMetric("browse_without_book");
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  if (initialClippingJump) {
    const bool epubTarget = initialClippingJump->bookType == "epub" && FsHelpers::hasEpubExtension(initialBookPath);
    const bool textTarget = initialClippingJump->bookType == "txt" && isTxtFile(initialBookPath);
    if (initialClippingJump->bookPath != initialBookPath || (!epubTarget && !textTarget)) {
      // The overload is only a transport. ReaderActivity remains the dispatch
      // boundary and never forwards a clipping target into a different reader
      // type.
      LOG_ERR("READER", "Rejected clipping jump for mismatched reader dispatch: %s", initialBookPath.c_str());
      initialClippingJump.reset();
    }
  }

  if (initialBookmarkJump) {
    const bool epubTarget = initialBookmarkJump->bookType == "epub" && FsHelpers::hasEpubExtension(initialBookPath);
    const bool textTarget = initialBookmarkJump->bookType == "txt" && isTxtFile(initialBookPath);
    const bool fixedTarget =
        initialBookmarkJump->bookType == "xtc" && isXtcFile(initialBookPath) && initialBookmarkJump->hasFixedPage;
    if (initialBookmarkJump->bookPath != initialBookPath || (!epubTarget && !textTarget && !fixedTarget)) {
      LOG_ERR("READER", "Rejected bookmark jump for mismatched reader dispatch: %s", initialBookPath.c_str());
      initialBookmarkJump.reset();
    }
  }

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    sdFontSystem.releaseLoadedFont(renderer);
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    sdFontSystem.releaseLoadedFont(renderer);
    if (!beginXtcLoad(initialBookPath)) {
      onGoBack();
      return;
    }
  } else if (isTxtFile(initialBookPath)) {
    if (!beginTxtLoad(initialBookPath)) {
      onGoBack();
      return;
    }
  } else {
    if (!beginEpubLoad(initialBookPath)) {
      onGoBack();
    }
  }
}

void ReaderActivity::loop() { pumpCooperativeOpen(); }

void ReaderActivity::onGoBack() {
  activityManager.cancelReaderOpenMetric("reader_open_failed_or_cancelled");
  if (activityManager.hasYourBooksReturnContext() || activityManager.hasSavedClippingsReturnContext()) {
    activityManager.returnFromReaderOrHome();
  } else {
    finish();
  }
}
