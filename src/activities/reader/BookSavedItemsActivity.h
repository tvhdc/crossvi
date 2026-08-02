#pragma once

#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "BookSavedItemsModel.h"
#include "activities/Activity.h"
#include "clippings/ClippingStore.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class BookSavedItemsActivity final : public Activity {
 public:
  enum class ReaderKind : uint8_t { Epub, Text, FixedLayout };

  BookSavedItemsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                         ClippingStore* clippingStore);
  BookSavedItemsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                         std::string bookTitle, std::string bookAuthor, ReaderKind readerKind,
                         ClippingStore* clippingStore = nullptr, uint32_t fixedPageCount = 0);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { List, DeleteConfirm };

  void loadBookmarks();
  void loadHighlightPreviews();
  void rebuildProjection();
  void changeTab(int direction);
  bool moveSelectionNext();
  bool moveSelectionPrevious();
  bool focusTabOrExit();
  void restoreNavigation();
  uint8_t tabCount() const;
  const char* currentTabLabel() const;
  void openSelected();
  void deleteSelected();
  void beginDelete();
  void cancel();
  bool saveBookmarks();
  bool bookmarkKindMatches(const BookmarkEntry& bookmark) const;
  std::string rowTitle(int index) const;
  std::string rowSubtitle(int index) const;
  UIIcon rowIcon(int index) const;
  std::vector<TabInfo> tabs() const;

  std::shared_ptr<Epub> epub_;
  std::string bookPath_;
  std::string bookTitle_;
  std::string bookAuthor_;
  ReaderKind readerKind_ = ReaderKind::Epub;
  ClippingStore* clippingStore_ = nullptr;
  uint32_t fixedPageCount_ = 0;

  std::vector<BookmarkEntry> bookmarks_;
  std::vector<std::string> highlightPreviews_;
  std::vector<BookSavedItemsModel::ItemRef> items_;
  BookSavedItemsModel::Tab tab_ = BookSavedItemsModel::Tab::All;
  ButtonNavigator navigator_;
  int selectedIndex_ = 0;
  bool tabFocused_ = true;
  bool bookmarksWritable_ = false;
  bool storageError_ = false;
  bool confirmPressSeen_ = false;
  bool ignoreConfirmRelease_ = false;
  bool longPressHandled_ = false;
  View view_ = View::List;
};
