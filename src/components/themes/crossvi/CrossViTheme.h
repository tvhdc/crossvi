#pragma once

#include "components/themes/BaseTheme.h"

struct HomeBookSummary;
namespace CrossViMetrics {
constexpr int HOME_RECENT_LIST_TILE_HEIGHT = 368;
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 80,
                                 .menuSpacing = 10,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 168,
                                 .homeCoverTileHeight = 320,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 12,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 6,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 6,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 8,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionHPadding = 16,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupTitleGap = 16,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = false,
                                 .optionPopupSelectionRadius = 6,
                                 .optionPopupSelectionLight = true,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}  // namespace CrossViMetrics

class CrossViTheme final : public BaseTheme {
 public:
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, std::span<const TabInfo> tabs, bool selected) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& rowBadge = nullptr, int pageAnchorIndex = -1,
                const std::function<int(int index)>& rowValueReservedWidth = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawHomeHeader(const GfxRenderer& renderer, Rect rect, const char* title) const override;
  Rect getHomeCoverCacheRect(Rect tileRect) const override;
  void drawHomeContent(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                       bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                       std::function<bool()> storeCoverBuffer, const HomeBookSummary& summary) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<const char*(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawHomeRecentList(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                          int selectedBookIndex) const;
  void drawHomeTripleCovers(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                            int currentBookIndex, bool bookSelected) const;
  void drawHomeCarousel(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                        int currentBookIndex, bool bookSelected, int buttonCount, int selectedButtonIndex,
                        const std::function<UIIcon(int index)>& buttonIcon, bool drawBookArea = true) const;
  bool showsFileIcons() const override { return true; }
};
