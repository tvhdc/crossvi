#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "InflateReader.h"

// The unchecksummed InflateReader path does not use these helpers, but
// tinflate.c exports the checksummed API from the same translation unit.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t previous) { return previous; }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t previous) { return previous; }

namespace {

// Raw DEFLATE with a dynamic Huffman block. The output is 5,000 bytes of the
// repeated 24-byte pattern below, which exercises the tree decoder used by
// compressed built-in fonts without putting a large fixture in the test.
constexpr std::array<uint8_t, 58> DYNAMIC_DEFLATE = {
    0xed, 0xc8, 0xb9, 0x01, 0x40, 0x30, 0x00, 0x00, 0xc0, 0xde, 0x14, 0xb1, 0x41, 0xfc, 0x54,
    0x66, 0x49, 0x90, 0x60, 0xff, 0x01, 0x2c, 0xa1, 0xbc, 0x2b, 0x2f, 0xe5, 0xe3, 0xbc, 0x4a,
    0xbd, 0x9f, 0x37, 0x76, 0xfd, 0x30, 0x4e, 0xf3, 0xb2, 0x6e, 0xa1, 0xdd, 0x9b, 0xe4, 0xbd,
    0xf7, 0xde, 0x7b, 0xef, 0xbd, 0xf7, 0xde, 0x7b, 0xef, 0xbd, 0xff, 0xed, 0x3f};
constexpr std::array<uint8_t, 24> OUTPUT_PATTERN = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', '0', '1',
                                                    '2', '3', '4', '5', '6', '7', '8', '9', ' ', '!', '?', '\n'};

TEST(FontInflateTest, DynamicHuffmanBlockInflatesWithoutLargeCallerStack) {
  constexpr size_t OUTPUT_SIZE = 5000;
  std::array<uint8_t, OUTPUT_SIZE> output{};

  InflateReader reader;
  ASSERT_TRUE(reader.init(false));
  reader.setSource(DYNAMIC_DEFLATE.data(), DYNAMIC_DEFLATE.size());
  ASSERT_TRUE(reader.read(output.data(), output.size()));

  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(output[i], OUTPUT_PATTERN[i % OUTPUT_PATTERN.size()]) << "byte " << i;
  }
}

}  // namespace
