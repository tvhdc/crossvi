#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "ReaderFontSize.h"
#include "SdCardFontRegistry.h"

namespace {

SdCardFontFamilyInfo family(std::initializer_list<uint8_t> sizes) {
  SdCardFontFamilyInfo result;
  result.name = "Test";
  for (const uint8_t size : sizes) {
    result.files.push_back({"/fonts/Test/Test_" + std::to_string(size) + ".cpfont", size, 0});
  }
  return result;
}

std::vector<uint8_t> selectedSizes(const SdCardFontFamilyInfo& fontFamily) {
  std::vector<uint8_t> result;
  for (uint8_t index = 0; index < ReaderFontSize::COUNT; ++index) {
    const auto* selected = fontFamily.findClosestReaderSize(index);
    result.push_back(selected ? selected->pointSize : 0);
  }
  return result;
}

TEST(ReaderFontSizeMappingTest, FullPackMapsEveryTargetExactly) {
  EXPECT_EQ(selectedSizes(family({12, 14, 16, 18, 20, 22, 24, 26, 28})),
            (std::vector<uint8_t>{12, 14, 16, 18, 20, 22, 24, 26, 28}));
}

TEST(ReaderFontSizeMappingTest, SparsePacksUseNearestSizeAndPreferSmallerTies) {
  EXPECT_EQ(selectedSizes(family({20, 22, 24, 26, 28})), (std::vector<uint8_t>{20, 20, 20, 20, 20, 22, 24, 26, 28}));
  EXPECT_EQ(selectedSizes(family({12, 16, 20, 24, 28})), (std::vector<uint8_t>{12, 12, 16, 16, 20, 20, 24, 24, 28}));
  EXPECT_EQ(selectedSizes(family({18, 24, 28})), (std::vector<uint8_t>{18, 18, 18, 18, 18, 24, 24, 24, 28}));
  EXPECT_EQ(selectedSizes(family({28})), (std::vector<uint8_t>(ReaderFontSize::COUNT, 28)));
}

TEST(ReaderFontSizeMappingTest, PhysicalPointTargetsAreNotQuantizedBeforeSelection) {
  EXPECT_EQ(family({17, 20}).findClosestPointSize(19)->pointSize, 20);
  EXPECT_EQ(family({17, 19}).findClosestPointSize(18)->pointSize, 17);
}

TEST(ReaderFontSizeMappingTest, FamilySwitchChoosesTheClosestRepresentablePhysicalSize) {
  const auto oldFamily = family({15});
  const auto newFamily = family({13, 15, 16});
  const auto* oldPhysical = oldFamily.findClosestReaderSize(ReaderFontSize::DEFAULT_INDEX);
  ASSERT_NE(oldPhysical, nullptr);
  ASSERT_EQ(oldPhysical->pointSize, 15);

  const int logicalSize = newFamily.findClosestReaderSizeEnum(oldPhysical->pointSize);
  ASSERT_EQ(logicalSize, 2);
  ASSERT_NE(newFamily.findClosestReaderSize(static_cast<uint8_t>(logicalSize)), nullptr);
  EXPECT_EQ(newFamily.findClosestReaderSize(static_cast<uint8_t>(logicalSize))->pointSize, 16);
}

TEST(ReaderFontSizeMappingTest, FileOrderAndDuplicateSizesDoNotChangeSelection) {
  const auto unordered = family({28, 12, 24, 16, 20, 14, 26, 18, 22});
  EXPECT_EQ(selectedSizes(unordered), (std::vector<uint8_t>{12, 14, 16, 18, 20, 22, 24, 26, 28}));

  const auto duplicate = family({18, 18, 24});
  EXPECT_EQ(duplicate.findClosestReaderSize(3)->pointSize, 18);
}

TEST(ReaderFontSizeMappingTest, DynamicChoicesExposePhysicalStandardSizesOnly) {
  EXPECT_EQ(family({20, 22, 24, 26, 28}).availableReaderSizeEnums(), (std::vector<uint8_t>{4, 5, 6, 7, 8}));
  EXPECT_EQ(family({12, 16, 20, 24, 28}).availableReaderSizeEnums(), (std::vector<uint8_t>{0, 2, 4, 6, 8}));
  EXPECT_EQ(family({18, 24, 28}).availableReaderSizeEnums(), (std::vector<uint8_t>{3, 6, 8}));
  EXPECT_EQ(family({28}).availableReaderSizeEnums(), (std::vector<uint8_t>{8}));
  EXPECT_EQ(family({13, 18}).availableReaderSizeEnums(), (std::vector<uint8_t>{0, 3}));
  EXPECT_EQ(family({13, 15}).availableReaderSizeEnums(), (std::vector<uint8_t>{0, 2}));
}

TEST(ReaderFontSizeMappingTest, PointSizeContractPreservesLegacyValues) {
  EXPECT_EQ(ReaderFontSize::pointSize(0), 12);
  EXPECT_EQ(ReaderFontSize::pointSize(1), 14);
  EXPECT_EQ(ReaderFontSize::pointSize(2), 16);
  EXPECT_EQ(ReaderFontSize::pointSize(3), 18);
  EXPECT_EQ(ReaderFontSize::closestIndex(21), 4);
  EXPECT_EQ(ReaderFontSize::closestIndex(27), 7);
}

}  // namespace
