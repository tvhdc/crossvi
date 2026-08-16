#pragma once
#include <Epub.h>
#include <Txt.h>
#include <Xtc.h>

#include <memory>
#include <optional>

#include "BookmarkEntry.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/reader/PerBookReaderSettings.h"

struct PerBookReaderSettings;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::optional<ClippingJumpResult> initialClippingJump;
  std::optional<SavedBookmarkJumpResult> initialBookmarkJump;
  std::string currentBookPath;  // Track current book path for navigation
  bool allowFastInitialRefresh = false;
  bool completionStatsWritableAtOpen = true;
  bool completionStatsAlreadyRecovered = false;
  bool readerOpenFeedbackAlreadyShown = false;
  ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default;
  std::optional<RawSourceIdentityHandoff> openingPreparedSourceIdentity;
  std::unique_ptr<Epub> openingPreparedEpub;
  std::unique_ptr<Xtc> openingPreparedXtc;
  std::unique_ptr<Txt> openingPreparedTxt;
  std::unique_ptr<Xtc> openingXtc;
  std::unique_ptr<Txt> openingTxt;
  uint32_t openingXtcStartedMs = 0;
  uint32_t openingTxtStartedMs = 0;
  std::unique_ptr<ZipSourceIdentityJob> openingEpubIdentityJob;
  uint32_t openingEpubIdentityStartedMs = 0;
  ZipFile::SourceIdentity openingEpubExpectedIdentity{};
  bool openingEpubFinalIdentityCheck = false;
  std::optional<GfxRenderer::FrameBufferLoan> openingEpubFrameBufferLoan;
  std::unique_ptr<Epub> openingEpub;
  bool openingEpubCacheInspection = false;
  uint32_t openingEpubCacheStartedMs = 0;
  uint32_t openingEpubIndexStartedMs = 0;
  PerBookReaderSettings openingGlobalSettings;
  PerBookReaderSettings openingBookSettings;
  bool openingSettingsWritable = true;
  bool openingEpubDeferCoverPreparation = false;
  // Non-static loaders can use the active display geometry for their derived cover cache.
  std::unique_ptr<Epub> loadEpub(const std::string& path, PerBookReaderSettings& globalSettings,
                                 PerBookReaderSettings& bookSettings, bool& settingsWritable,
                                 bool& deferCoverPreparation, const ZipFile::SourceIdentity& verifiedEpubIdentity,
                                 BookMetadataCache::LoadStepResult& cacheStepResult,
                                 std::unique_ptr<Epub> preparedEpub = nullptr);
  bool beginEpubLoad(const std::string& path);
  bool finishEpubLoad(const ZipFile::SourceIdentity& verifiedEpubIdentity);
  bool finishEpubCacheInspection(BookMetadataCache::LoadStepResult result);
  bool beginXtcLoad(const std::string& path);
  bool finishXtcLoad(bool& deferCoverPreparation);
  bool beginTxtLoad(const std::string& path);
  bool finishTxtLoad();
  void pumpCooperativeOpen();
  void cancelCooperativeOpen();
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);
  bool skipDerivedCoverCacheBuild() const;
  void validateInitialBookmarkJump(BookmarkEntry::PositionKind kind);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub, PerBookReaderSettings globalSettings,
                        PerBookReaderSettings bookSettings, bool settingsWritable, bool deferCoverPreparation);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc, bool deferCoverPreparation);
  void onGoToTxtReader(std::unique_ptr<Txt> txt, PerBookReaderSettings globalSettings,
                       PerBookReaderSettings bookSettings, bool settingsWritable);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();
  int initialRefreshCountdown() const;

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          std::optional<ClippingJumpResult> initialClippingJump = std::nullopt,
                          const ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        initialClippingJump(std::move(initialClippingJump)),
        openOrigin(openOrigin) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                 const bool allowFastInitialRefresh, const ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default,
                 const bool completionStatsAlreadyRecovered = false, const bool readerOpenFeedbackAlreadyShown = false,
                 const RawSourceIdentityHandoff* const preparedSourceIdentity = nullptr)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        allowFastInitialRefresh(allowFastInitialRefresh),
        completionStatsAlreadyRecovered(completionStatsAlreadyRecovered),
        readerOpenFeedbackAlreadyShown(readerOpenFeedbackAlreadyShown),
        openOrigin(openOrigin),
        openingPreparedSourceIdentity(
            preparedSourceIdentity ? std::optional<RawSourceIdentityHandoff>(*preparedSourceIdentity) : std::nullopt) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> preparedEpub,
                 const ReaderOpenOrigin openOrigin, const bool completionStatsAlreadyRecovered,
                 const bool readerOpenFeedbackAlreadyShown)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(preparedEpub ? preparedEpub->getPath() : std::string{}),
        completionStatsAlreadyRecovered(completionStatsAlreadyRecovered),
        readerOpenFeedbackAlreadyShown(readerOpenFeedbackAlreadyShown),
        openOrigin(openOrigin),
        openingPreparedEpub(std::move(preparedEpub)) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> preparedXtc,
                 const ReaderOpenOrigin openOrigin, const bool completionStatsAlreadyRecovered,
                 const bool readerOpenFeedbackAlreadyShown)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(preparedXtc ? preparedXtc->getPath() : std::string{}),
        completionStatsAlreadyRecovered(completionStatsAlreadyRecovered),
        readerOpenFeedbackAlreadyShown(readerOpenFeedbackAlreadyShown),
        openOrigin(openOrigin),
        openingPreparedXtc(std::move(preparedXtc)) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> preparedTxt,
                 const ReaderOpenOrigin openOrigin, const bool completionStatsAlreadyRecovered,
                 const bool readerOpenFeedbackAlreadyShown)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(preparedTxt ? preparedTxt->getPath() : std::string{}),
        completionStatsAlreadyRecovered(completionStatsAlreadyRecovered),
        readerOpenFeedbackAlreadyShown(readerOpenFeedbackAlreadyShown),
        openOrigin(openOrigin),
        openingPreparedTxt(std::move(preparedTxt)) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                 SavedBookmarkJumpResult initialBookmarkJump,
                 const ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        initialBookmarkJump(std::move(initialBookmarkJump)),
        openOrigin(openOrigin) {}
  ~ReaderActivity() override;
  void onEnter() override;
  void loop() override;
  bool skipLoopDelay() override { return openingXtc || openingTxt || openingEpubIdentityJob || openingEpub; }
  bool isReaderActivity() const override { return true; }
};
