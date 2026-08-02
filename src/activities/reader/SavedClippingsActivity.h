#pragma once

#include <string>

#include "SavedClippingsModel.h"
#include "activities/Activity.h"
#include "clippings/ClippingStore.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class SavedClippingsActivity final : public Activity {
 public:
  explicit SavedClippingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  std::optional<SavedClippingsReturnState> returnState = std::nullopt)
      : Activity("SavedClippings", renderer, mappedInput), pendingReturnState(std::move(returnState)) {}

  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  int rowCount() const { return static_cast<int>(savedCatalog_.entries.size()) + 1; }
  bool exportAvailable() const { return SavedClippingsModel::canExport(clippingCatalogLoadResult_, clippingCatalog_); }

  void reloadCatalog(bool clearNotice);
  void openSelectedBook();
  void handleSavedBookResult(const ActivityResult& result);
  void handleClippingResult(const ClippingJumpResult& jump);
  void exportAll();

  std::string rowTitle(int index) const;
  std::string rowSubtitle(int index) const;
  UIIcon rowIcon(int index) const;
  std::string statusText() const;

  ClippingStore::Catalog clippingCatalog_;
  ClippingStore::CatalogLoadResult clippingCatalogLoadResult_ = ClippingStore::CatalogLoadResult::DirectoryMissing;
  BookmarkCatalog::Catalog bookmarkCatalog_;
  BookmarkCatalog::LoadResult bookmarkCatalogLoadResult_ = BookmarkCatalog::LoadResult::DirectoryMissing;
  SavedClippingsModel::CombinedCatalog savedCatalog_;
  ClippingStore openedStore_;
  std::string openedBookPath_;
  std::string openedBookType_;
  std::optional<SavedClippingsReturnState> pendingReturnState;
  std::string restoredBookPath_;
  ButtonNavigator navigator_;
  int selectedIndex_ = 0;
  std::string notice_;
};
