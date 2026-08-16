#include <gtest/gtest.h>

#include <array>
#include <map>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "FontCacheManager.h"
#include "FontDecompressor.h"
#include "SdCardFont.h"

namespace {

constexpr std::array<uint8_t, 7> COMPRESSED_BITMAP = {0x01, 0x02, 0x00, 0xFD, 0xFF, 0x1B, 0xE4};
constexpr std::array<EpdGlyph, 2> GLYPHS = {
    EpdGlyph{4, 1, 4 << 4, 0, 1, 1, 0},
    EpdGlyph{4, 1, 4 << 4, 0, 1, 1, 1},
};
constexpr std::array<EpdUnicodeInterval, 1> INTERVALS = {EpdUnicodeInterval{'A', 'B', 0}};
constexpr std::array<EpdFontGroup, 1> GROUPS = {
    EpdFontGroup{0, COMPRESSED_BITMAP.size(), 2, 2, 0},
};

EpdFontData makeFont() {
  EpdFontData font{};
  font.bitmap = COMPRESSED_BITMAP.data();
  font.glyph = GLYPHS.data();
  font.intervals = INTERVALS.data();
  font.intervalCount = INTERVALS.size();
  font.advanceY = 8;
  font.is2Bit = true;
  font.groups = GROUPS.data();
  font.groupCount = GROUPS.size();
  return font;
}

TEST(FontCacheManagerTest, PrewarmsOnlyTextRenderedWithEachStyle) {
  EpdFontData fontData = makeFont();
  EpdFont regular(&fontData);
  EpdFont bold(&fontData);
  const std::map<int, EpdFontFamily> fonts = {{7, EpdFontFamily(&regular, &bold)}};
  const std::map<int, SdCardFont*> sdFonts;
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());
  FontCacheManager manager(fonts, sdFonts);
  manager.setFontDecompressor(&decompressor);

  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("A", 7, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }
  const uint32_t oneGlyphLookupBytes = decompressor.getStats().pageGlyphsBytes;
  ASSERT_GT(oneGlyphLookupBytes, 0U);

  manager.clearCache();
  manager.resetStats();
  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("A", 7, EpdFontFamily::REGULAR);
    manager.recordText("B", 7, EpdFontFamily::BOLD);
    scope.endScanAndPrewarm();
  }

  EXPECT_EQ(decompressor.getStats().pageGlyphsBytes, oneGlyphLookupBytes * 2U);
}

TEST(FontCacheManagerTest, PrewarmsRenderedUppercaseForSyntheticSmallCaps) {
  EpdFontData fontData = makeFont();
  EpdFont regular(&fontData);
  const std::map<int, EpdFontFamily> fonts = {{7, EpdFontFamily(&regular)}};
  const std::map<int, SdCardFont*> sdFonts;
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());
  FontCacheManager manager(fonts, sdFonts);
  manager.setFontDecompressor(&decompressor);

  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("a", 7, static_cast<EpdFontFamily::Style>(EpdFontFamily::REGULAR | EpdFontFamily::SMALL_CAPS));
    scope.endScanAndPrewarm();
  }

  EXPECT_GT(decompressor.getStats().pageBufferBytes, 0U);
}

TEST(FontCacheManagerTest, PrewarmsNegativeBuiltInFontIds) {
  EpdFontData fontData = makeFont();
  EpdFont regular(&fontData);
  const std::map<int, EpdFontFamily> fonts = {{-7, EpdFontFamily(&regular)}};
  const std::map<int, SdCardFont*> sdFonts;
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());
  FontCacheManager manager(fonts, sdFonts);
  manager.setFontDecompressor(&decompressor);

  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("A", -7, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }

  EXPECT_GT(decompressor.getStats().pageBufferBytes, 0U);
}

}  // namespace
