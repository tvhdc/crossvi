#include <gtest/gtest.h>

#include "Epub/Epub/blocks/BlockStyle.h"

TEST(BlockStyle, StripsOnlyVerticalInsetsForInlineBreaks) {
  BlockStyle style;
  style.alignment = CssTextAlign::Center;
  style.marginTop = 11;
  style.marginBottom = 12;
  style.marginLeft = 13;
  style.marginRight = 14;
  style.paddingTop = 21;
  style.paddingBottom = 22;
  style.paddingLeft = 23;
  style.paddingRight = 24;
  style.textAlignDefined = true;

  const BlockStyle result = style.withoutTop().withoutBottom();

  EXPECT_EQ(result.marginTop, 0);
  EXPECT_EQ(result.marginBottom, 0);
  EXPECT_EQ(result.paddingTop, 0);
  EXPECT_EQ(result.paddingBottom, 0);
  EXPECT_EQ(result.marginLeft, style.marginLeft);
  EXPECT_EQ(result.marginRight, style.marginRight);
  EXPECT_EQ(result.paddingLeft, style.paddingLeft);
  EXPECT_EQ(result.paddingRight, style.paddingRight);
  EXPECT_EQ(result.alignment, style.alignment);
  EXPECT_EQ(result.textAlignDefined, style.textAlignDefined);
}

TEST(BlockStyle, SaturatesNestedInsetsInsteadOfWrapping) {
  BlockStyle parent;
  parent.marginLeft = 30000;
  parent.paddingBottom = 30000;
  BlockStyle child;
  child.marginLeft = 30000;
  child.paddingBottom = 30000;

  const BlockStyle horizontal = parent.getCombinedBlockStyle(child, BlockStyle::CombineAxis::Horizontal);
  EXPECT_EQ(horizontal.marginLeft, INT16_MAX);

  const BlockStyle vertical = parent.getCombinedBlockStyle(child, BlockStyle::CombineAxis::Vertical);
  EXPECT_EQ(vertical.paddingBottom, INT16_MAX);

  BlockStyle insets;
  insets.marginLeft = INT16_MAX;
  insets.paddingLeft = 1;
  insets.marginRight = INT16_MAX;
  EXPECT_EQ(insets.leftInset(), INT16_MAX);
  EXPECT_EQ(insets.totalHorizontalInset(), INT16_MAX);
}
