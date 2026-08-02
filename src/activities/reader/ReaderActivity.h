#pragma once
#include <memory>
#include <optional>

#include "BookmarkEntry.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;
struct PerBookReaderSettings;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::optional<ClippingJumpResult> initialClippingJump;
  std::optional<SavedBookmarkJumpResult> initialBookmarkJump;
  std::string currentBookPath;  // Track current book path for navigation
  bool allowFastInitialRefresh = false;
  ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default;
  // Non-static loaders can use the active display geometry for their derived cover cache.
  std::unique_ptr<Epub> loadEpub(const std::string& path, PerBookReaderSettings& globalSettings,
                                 PerBookReaderSettings& bookSettings, bool& settingsWritable,
                                 bool& deferCoverPreparation);
  std::unique_ptr<Xtc> loadXtc(const std::string& path);
  std::unique_ptr<Txt> loadTxt(const std::string& path, PerBookReaderSettings& globalSettings,
                               PerBookReaderSettings& bookSettings, bool& settingsWritable);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);
  bool skipDerivedCoverCacheBuild() const;
  void validateInitialBookmarkJump(BookmarkEntry::PositionKind kind);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub, PerBookReaderSettings globalSettings,
                        PerBookReaderSettings bookSettings, bool settingsWritable, bool deferCoverPreparation);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
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
                 const bool allowFastInitialRefresh, const ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        allowFastInitialRefresh(allowFastInitialRefresh),
        openOrigin(openOrigin) {}
  ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                 SavedBookmarkJumpResult initialBookmarkJump,
                 const ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        initialBookmarkJump(std::move(initialBookmarkJump)),
        openOrigin(openOrigin) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
