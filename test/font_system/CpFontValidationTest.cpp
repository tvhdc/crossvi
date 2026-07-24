#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "SdCardFont.h"
#include "VietnameseFontContract.h"

namespace {

constexpr size_t kHeaderSize = 32;
constexpr size_t kTocSize = 32;

void putU16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
  bytes[at] = value & 0xFF;
  bytes[at + 1] = value >> 8;
}

void putU32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
}

std::vector<uint8_t> makeFont(const std::vector<uint8_t>& styleIds = {0}, uint32_t first = 0x20,
                              uint32_t last = 0x7E,
                              const std::vector<std::pair<uint32_t, uint32_t>>& ligatures = {}) {
  const uint32_t glyphCount = last - first + 1;
  const uint32_t dataStart = kHeaderSize + static_cast<uint32_t>(styleIds.size()) * kTocSize;
  const uint32_t styleBytes = 12 + glyphCount * 16 + static_cast<uint32_t>(ligatures.size()) * 8;
  std::vector<uint8_t> bytes(dataStart + styleBytes * styleIds.size(), 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  putU16(bytes, 8, CPFONT_VERSION);
  putU16(bytes, 10, 1);
  bytes[12] = static_cast<uint8_t>(styleIds.size());

  for (size_t i = 0; i < styleIds.size(); ++i) {
    const size_t toc = kHeaderSize + i * kTocSize;
    const uint32_t data = dataStart + static_cast<uint32_t>(i) * styleBytes;
    bytes[toc] = styleIds[i];
    putU32(bytes, toc + 4, 1);
    putU32(bytes, toc + 8, glyphCount);
    bytes[toc + 12] = 20;
    putU16(bytes, toc + 13, 15);
    putU16(bytes, toc + 15, static_cast<uint16_t>(-5));
    bytes[toc + 23] = static_cast<uint8_t>(ligatures.size());
    putU32(bytes, toc + 24, data);

    putU32(bytes, data, first);
    putU32(bytes, data + 4, last);
    putU32(bytes, data + 8, 0);
    for (uint32_t glyph = 0; glyph < glyphCount; ++glyph) {
      const size_t entry = data + 12 + glyph * 16;
      putU16(bytes, entry + 2, 8 * 16);
    }
    size_t ligatureOffset = data + 12 + glyphCount * 16;
    for (const auto& pair : ligatures) {
      putU32(bytes, ligatureOffset, pair.first);
      putU32(bytes, ligatureOffset + 4, pair.second);
      ligatureOffset += 8;
    }
  }
  return bytes;
}

std::vector<uint8_t> makeSparseFont(std::vector<uint32_t> codepoints, const std::vector<uint8_t>& styleIds = {0}) {
  std::sort(codepoints.begin(), codepoints.end());
  codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
  struct Interval {
    uint32_t first;
    uint32_t last;
    uint32_t offset;
  };
  std::vector<Interval> intervals;
  for (const uint32_t cp : codepoints) {
    if (!intervals.empty() && cp == intervals.back().last + 1) {
      intervals.back().last = cp;
    } else {
      intervals.push_back({cp, cp, static_cast<uint32_t>(intervals.empty() ? 0 :
          intervals.back().offset + intervals.back().last - intervals.back().first + 1)});
    }
  }

  const uint32_t glyphCount = static_cast<uint32_t>(codepoints.size());
  const uint32_t dataStart = kHeaderSize + static_cast<uint32_t>(styleIds.size()) * kTocSize;
  const uint32_t styleBytes = static_cast<uint32_t>(intervals.size()) * 12 + glyphCount * 16;
  std::vector<uint8_t> bytes(dataStart + styleBytes * styleIds.size(), 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  putU16(bytes, 8, CPFONT_VERSION);
  putU16(bytes, 10, 1);
  bytes[12] = static_cast<uint8_t>(styleIds.size());
  for (size_t styleIndex = 0; styleIndex < styleIds.size(); ++styleIndex) {
    const size_t toc = kHeaderSize + styleIndex * kTocSize;
    const uint32_t data = dataStart + static_cast<uint32_t>(styleIndex) * styleBytes;
    bytes[toc] = styleIds[styleIndex];
    putU32(bytes, toc + 4, static_cast<uint32_t>(intervals.size()));
    putU32(bytes, toc + 8, glyphCount);
    bytes[toc + 12] = 20;
    putU16(bytes, toc + 13, 15);
    putU16(bytes, toc + 15, static_cast<uint16_t>(-5));
    putU32(bytes, toc + 24, data);
    size_t cursor = data;
    for (const auto& interval : intervals) {
      putU32(bytes, cursor, interval.first);
      putU32(bytes, cursor + 4, interval.last);
      putU32(bytes, cursor + 8, interval.offset);
      cursor += 12;
    }
    for (uint32_t glyph = 0; glyph < glyphCount; ++glyph) {
      putU16(bytes, cursor + glyph * 16 + 2, 8 * 16);
    }
  }
  return bytes;
}

std::vector<uint32_t> vietnameseContractCodepoints() {
  std::vector<uint32_t> codepoints;
  for (const auto& range : VietnameseFontContract::REQUIRED_RANGES) {
    for (uint32_t cp = range.first; cp <= range.last; ++cp) codepoints.push_back(cp);
  }
  codepoints.insert(codepoints.end(), std::begin(VietnameseFontContract::REQUIRED_SINGLETONS),
                    std::end(VietnameseFontContract::REQUIRED_SINGLETONS));
  return codepoints;
}

class CpFontValidationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() / "crossvi-font-validation.cpfont";
  }
  void TearDown() override { std::filesystem::remove(path_); }

  bool load(const std::vector<uint8_t>& bytes, SdCardFont* out = nullptr) {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (out) return out->load(path_.c_str());
    SdCardFont font;
    return font.load(path_.c_str());
  }

  std::filesystem::path path_;
};

TEST_F(CpFontValidationTest, AcceptsRegularOnlyAndFallsBackStyles) {
  SdCardFont font;
  ASSERT_TRUE(load(makeFont(), &font));
  EXPECT_FALSE(font.supportsVietnamese());
  EXPECT_TRUE(font.hasStyle(0));
  EXPECT_EQ(font.resolveStyle(1), 0);
  EXPECT_EQ(font.resolveStyle(2), 0);
  EXPECT_EQ(font.resolveStyle(3), 0);
}

TEST_F(CpFontValidationTest, RejectsBadMagicAndNewerVersion) {
  auto bytes = makeFont();
  bytes[0] = 'X';
  EXPECT_FALSE(load(bytes));
  bytes = makeFont();
  putU16(bytes, 8, CPFONT_VERSION + 1);
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsTruncatedHeaderAndToc) {
  auto bytes = makeFont();
  bytes.resize(kHeaderSize - 1);
  EXPECT_FALSE(load(bytes));
  bytes = makeFont();
  bytes.resize(kHeaderSize + kTocSize - 1);
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsDuplicateStylesAndMissingRegular) {
  EXPECT_FALSE(load(makeFont({0, 0})));
  EXPECT_FALSE(load(makeFont({1})));
}

TEST_F(CpFontValidationTest, RejectsZeroStyleCountAndUnknownStyleId) {
  auto bytes = makeFont();
  bytes[12] = 0;
  EXPECT_FALSE(load(bytes));
  bytes = makeFont();
  bytes[kHeaderSize] = 4;
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsMalformedIntervalLayout) {
  auto bytes = makeFont();
  const size_t interval = kHeaderSize + kTocSize;
  putU32(bytes, interval, 0x7E);
  putU32(bytes, interval + 4, 0x20);
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsUnreasonableCountsAndOffsetOverflow) {
  auto bytes = makeFont();
  putU32(bytes, kHeaderSize + 4, 4097);
  EXPECT_FALSE(load(bytes));
  bytes = makeFont();
  putU32(bytes, kHeaderSize + 24, 0xFFFFFFF0U);
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsGlyphBitmapOutsideFile) {
  auto bytes = makeFont();
  const size_t firstGlyph = kHeaderSize + kTocSize + 12;
  bytes[firstGlyph] = 2;
  bytes[firstGlyph + 1] = 2;
  putU16(bytes, firstGlyph + 10, 2);
  putU32(bytes, firstGlyph + 12, 1);
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, RejectsTruncatedGlyphMetadata) {
  auto bytes = makeFont();
  bytes.pop_back();
  EXPECT_FALSE(load(bytes));
}

TEST_F(CpFontValidationTest, ContentHashChangesWhenGlyphMetricsChange) {
  SdCardFont first;
  ASSERT_TRUE(load(makeFont(), &first));
  const uint32_t firstHash = first.contentHash();

  auto bytes = makeFont();
  const size_t firstGlyph = kHeaderSize + kTocSize + 12;
  putU16(bytes, firstGlyph + 2, 9 * 16);
  SdCardFont second;
  ASSERT_TRUE(load(bytes, &second));
  EXPECT_NE(second.contentHash(), firstHash);
}

TEST_F(CpFontValidationTest, ConverterFixturePassesProductionParser) {
  const char* fixture = std::getenv("CROSSVI_CPFONT_FIXTURE");
  if (!fixture || fixture[0] == '\0') GTEST_SKIP() << "CROSSVI_CPFONT_FIXTURE not set";
  SdCardFont font;
  ASSERT_TRUE(font.load(fixture));
  EXPECT_TRUE(font.supportsVietnamese());
  for (uint8_t style = 0; style < 4; ++style) EXPECT_TRUE(font.hasStyle(style));
}

TEST_F(CpFontValidationTest, CompleteRegularOnlyVietnameseFontIsAcceptedWithStyleFallback) {
  SdCardFont font;
  ASSERT_TRUE(load(makeSparseFont(vietnameseContractCodepoints()), &font));
  EXPECT_TRUE(font.supportsVietnamese());
  for (uint8_t style = 0; style < 4; ++style) EXPECT_EQ(font.resolveStyle(style), 0);
}

TEST_F(CpFontValidationTest, MissingCommonLatin1VietnameseLettersIsDetected) {
  for (const uint32_t missing : {0x00C1U, 0x00E9U, 0x00CDU, 0x00F5U, 0x00DAU, 0x00FDU}) {
    auto codepoints = vietnameseContractCodepoints();
    codepoints.erase(std::remove(codepoints.begin(), codepoints.end(), missing), codepoints.end());
    SdCardFont font;
    ASSERT_TRUE(load(makeSparseFont(codepoints), &font)) << std::hex << missing;
    EXPECT_FALSE(font.supportsVietnamese()) << std::hex << missing;
  }
}

TEST_F(CpFontValidationTest, MissingNfdPunctuationCurrencyOrReplacementIsDetected) {
  for (const uint32_t missing : {0x0309U, 0x031BU, 0x201CU, 0x2026U, 0x20ABU, 0x20ACU, 0xFFFDU}) {
    auto codepoints = vietnameseContractCodepoints();
    codepoints.erase(std::remove(codepoints.begin(), codepoints.end(), missing), codepoints.end());
    SdCardFont font;
    ASSERT_TRUE(load(makeSparseFont(codepoints), &font)) << std::hex << missing;
    EXPECT_FALSE(font.supportsVietnamese()) << std::hex << missing;
  }
}

TEST_F(CpFontValidationTest, AcceptsStrictlySortedLigatureTable) {
  EXPECT_TRUE(load(makeFont({0}, 0x20, 0x7E, {{0x00660069U, 0xFB01U}, {0x0066006CU, 0xFB02U}})));
}

TEST_F(CpFontValidationTest, RejectsUnsortedOrDuplicateLigaturePairs) {
  EXPECT_FALSE(load(makeFont({0}, 0x20, 0x7E, {{0x0066006CU, 0xFB02U}, {0x00660069U, 0xFB01U}})));
  EXPECT_FALSE(load(makeFont({0}, 0x20, 0x7E, {{0x00660069U, 0xFB01U}, {0x00660069U, 0xFB01U}})));
}

TEST_F(CpFontValidationTest, RejectsTruncatedOrOutOfBoundsLigatureTable) {
  auto bytes = makeFont({0}, 0x20, 0x7E, {{0x00660069U, 0xFB01U}});
  bytes.pop_back();
  EXPECT_FALSE(load(bytes));
  bytes = makeFont();
  bytes[kHeaderSize + 23] = 255;
  EXPECT_FALSE(load(bytes));
}

}  // namespace
