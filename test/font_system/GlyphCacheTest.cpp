#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "FontDecompressor.h"

namespace {

// One final uncompressed DEFLATE block whose two output bytes are the packed
// 2-bit bitmaps for glyphs A and B.
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

TEST(GlyphCacheTest, ReusesEightKiBRingAcrossPageCacheRelease) {
  static_assert(FontDecompressor::GLYPH_CACHE_BYTES == 8 * 1024);

  EpdFontData font = makeFont();
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());

  ASSERT_EQ(decompressor.prewarmCache(&font, "AB"), 0);
  EXPECT_EQ(decompressor.getStats().decompressions, 1U);
  ASSERT_NE(decompressor.getBitmap(&font, &GLYPHS[0], 0), nullptr);
  EXPECT_EQ(*decompressor.getBitmap(&font, &GLYPHS[0], 0), 0x1B);

  decompressor.releasePageCache();
  decompressor.resetStats();

  ASSERT_EQ(decompressor.prewarmCache(&font, "AB"), 0);
  EXPECT_EQ(decompressor.getStats().decompressions, 0U);
  EXPECT_EQ(decompressor.getStats().persistentCacheHits, 2U);
  EXPECT_EQ(*decompressor.getBitmap(&font, &GLYPHS[1], 1), 0xE4);

  decompressor.clearCache();
  decompressor.resetStats();

  ASSERT_EQ(decompressor.prewarmCache(&font, "AB"), 0);
  EXPECT_EQ(decompressor.getStats().decompressions, 1U);
  EXPECT_EQ(decompressor.getStats().persistentCacheHits, 0U);
}

}  // namespace
