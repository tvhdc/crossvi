#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "BookmarkEntry.h"
#include "clippings/ClippingStore.h"

namespace BookSavedItemsModel {

enum class Kind : uint8_t { Bookmark, Highlight };
enum class Tab : uint8_t { All, Bookmarks, Highlights };

struct ItemRef {
  Kind kind = Kind::Bookmark;
  uint16_t sourceIndex = 0;
  uint64_t position = 0;
};

inline bool visibleInTab(const ItemRef& item, const Tab tab) {
  return tab == Tab::All || (tab == Tab::Bookmarks && item.kind == Kind::Bookmark) ||
         (tab == Tab::Highlights && item.kind == Kind::Highlight);
}

inline uint64_t bookmarkPosition(const BookmarkEntry& bookmark) {
  switch (bookmark.positionKind) {
    case BookmarkEntry::PositionKind::Text:
      return bookmark.byteOffset;
    case BookmarkEntry::PositionKind::FixedLayout:
      return bookmark.pageIndex;
    case BookmarkEntry::PositionKind::Epub:
    default:
      return (static_cast<uint64_t>(bookmark.computedSpineIndex) << 32U) |
             (static_cast<uint64_t>(bookmark.computedChapterProgress) << 16U);
  }
}

inline uint64_t highlightPosition(const ClippingCodec::ClippingMetadata& clipping) {
  if (clipping.hasTextAnchor) return clipping.textSourceStart;
  return (static_cast<uint64_t>(clipping.spineIndex) << 32U) | (static_cast<uint64_t>(clipping.startPage) << 16U) |
         clipping.startWordIndex;
}

inline void sortItems(std::vector<ItemRef>& items) {
  std::stable_sort(items.begin(), items.end(), [](const ItemRef& left, const ItemRef& right) {
    if (left.position != right.position) return left.position < right.position;
    if (left.kind != right.kind) return left.kind == Kind::Bookmark;
    return left.sourceIndex < right.sourceIndex;
  });
}

inline std::vector<ItemRef> project(const std::vector<BookmarkEntry>& bookmarks, const ClippingStore* clippings,
                                    const Tab tab) {
  std::vector<ItemRef> result;
  result.reserve(bookmarks.size() + (clippings ? clippings->size() : 0));
  for (size_t index = 0; index < bookmarks.size(); ++index) {
    ItemRef item{Kind::Bookmark, static_cast<uint16_t>(index), bookmarkPosition(bookmarks[index])};
    if (visibleInTab(item, tab)) result.push_back(item);
  }
  if (clippings) {
    for (size_t index = 0; index < clippings->size(); ++index) {
      const auto* clipping = clippings->at(index);
      if (!clipping) continue;
      ItemRef item{Kind::Highlight, static_cast<uint16_t>(index), highlightPosition(*clipping)};
      if (visibleInTab(item, tab)) result.push_back(item);
    }
  }
  sortItems(result);
  return result;
}

}  // namespace BookSavedItemsModel
