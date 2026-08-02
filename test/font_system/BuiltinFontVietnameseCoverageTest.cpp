#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "EpdFont.h"
#include "builtinFonts/all.h"

namespace {

constexpr std::array<uint32_t, 18> kCombiningAndPunctuation = {
    0x0300, 0x0301, 0x0302, 0x0303, 0x0306, 0x0309, 0x031B, 0x0323, 0x2013,
    0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AB, 0xFFFD,
};

struct FontCase {
  const char* name;
  const EpdFontData* data;
};

#define FONT_CASE(name) \
  FontCase { #name, &name }

const FontCase kReaderFonts[] = {
    FONT_CASE(notoserif_12_regular),    FONT_CASE(notoserif_12_bold),       FONT_CASE(notoserif_12_italic),
    FONT_CASE(notoserif_12_bolditalic), FONT_CASE(notoserif_14_regular),    FONT_CASE(notoserif_14_bold),
    FONT_CASE(notoserif_14_italic),     FONT_CASE(notoserif_14_bolditalic), FONT_CASE(notoserif_16_regular),
    FONT_CASE(notoserif_16_bold),       FONT_CASE(notoserif_16_italic),     FONT_CASE(notoserif_16_bolditalic),
    FONT_CASE(notoserif_18_regular),    FONT_CASE(notoserif_18_bold),       FONT_CASE(notoserif_18_italic),
    FONT_CASE(notoserif_18_bolditalic), FONT_CASE(notosans_12_regular),     FONT_CASE(notosans_12_bold),
    FONT_CASE(notosans_12_italic),      FONT_CASE(notosans_12_bolditalic),  FONT_CASE(notosans_14_regular),
    FONT_CASE(notosans_14_bold),        FONT_CASE(notosans_14_italic),      FONT_CASE(notosans_14_bolditalic),
    FONT_CASE(notosans_16_regular),     FONT_CASE(notosans_16_bold),        FONT_CASE(notosans_16_italic),
    FONT_CASE(notosans_16_bolditalic),  FONT_CASE(notosans_18_regular),     FONT_CASE(notosans_18_bold),
    FONT_CASE(notosans_18_italic),      FONT_CASE(notosans_18_bolditalic),
};

const FontCase kUiFonts[] = {
    FONT_CASE(ubuntu_10_regular),
    FONT_CASE(ubuntu_10_bold),
    FONT_CASE(ubuntu_12_regular),
    FONT_CASE(ubuntu_12_bold),
};

void expectVietnameseCoverage(const FontCase& fontCase) {
  const EpdFont font(fontCase.data);
  for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) {
    EXPECT_NE(font.getGlyph(cp), nullptr) << fontCase.name << " U+" << std::hex << cp;
  }
  for (uint32_t cp = 'a'; cp <= 'z'; ++cp) {
    EXPECT_NE(font.getGlyph(cp), nullptr) << fontCase.name << " U+" << std::hex << cp;
  }
  for (uint32_t cp = 0x1EA0; cp <= 0x1EF9; ++cp) {
    EXPECT_NE(font.getGlyph(cp), nullptr) << fontCase.name << " U+" << std::hex << cp;
  }
  for (const uint32_t cp :
       {0x0102U, 0x0103U, 0x0110U, 0x0111U, 0x0128U, 0x0129U, 0x0168U, 0x0169U, 0x01A0U, 0x01A1U, 0x01AFU, 0x01B0U}) {
    EXPECT_NE(font.getGlyph(cp), nullptr) << fontCase.name << " U+" << std::hex << cp;
  }
  for (const uint32_t cp : kCombiningAndPunctuation) {
    EXPECT_NE(font.getGlyph(cp), nullptr) << fontCase.name << " U+" << std::hex << cp;
  }
}

TEST(BuiltinFontVietnameseCoverage, EveryReaderSizeAndStyle) {
  for (const auto& fontCase : kReaderFonts) expectVietnameseCoverage(fontCase);
}

TEST(BuiltinFontVietnameseCoverage, UbuntuUiRegularAndBold) {
  for (const auto& fontCase : kUiFonts) expectVietnameseCoverage(fontCase);
}

#undef FONT_CASE

}  // namespace
