#include <HalStorage.h>
#include <JPEGDEC.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "JpegToBmpConverter.h"

namespace {

class ByteSink final : public Print {
 public:
  using Print::write;
  size_t write(const uint8_t* data, const size_t length) override {
    bytes.insert(bytes.end(), data, data + length);
    return length;
  }

  std::vector<uint8_t> bytes;
};

class ShortWriteSink final : public Print {
 public:
  explicit ShortWriteSink(const size_t failCall) : failCall_(failCall) {}
  using Print::write;
  size_t write(const uint8_t*, const size_t length) override {
    ++calls;
    return calls == failCall_ && length > 0 ? length - 1 : length;
  }

  size_t calls = 0;

 private:
  size_t failCall_;
};

HalFile openInput() {
  Storage.reset();
  Storage.setFile("/image.jpg", {0xFF, 0xD8, 0xFF, 0xD9});
  HalFile input;
  EXPECT_TRUE(Storage.openFileForRead("JPG", "/image.jpg", input));
  return input;
}

TEST(JpegConverterSafetyTest, WritesACompleteOneBitBmp) {
  HalFile input = openInput();
  ByteSink output;
  ASSERT_TRUE(input);
  ASSERT_EQ(input.fileSize64(), 4U);

  ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, 2, 1, false));
  ASSERT_EQ(output.bytes.size(), 66U);
  EXPECT_EQ(output.bytes[0], 'B');
  EXPECT_EQ(output.bytes[1], 'M');
}

TEST(JpegConverterSafetyTest, RejectsShortWriteInBmpHeader) {
  HalFile input = openInput();
  ShortWriteSink output(1);
  JPEGDEC::resetDecodeCalls();

  EXPECT_FALSE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, 2, 1, false));
  EXPECT_EQ(JPEGDEC::decodeCalls(), 0U);
}

TEST(JpegConverterSafetyTest, RejectsShortWriteInPixelRows) {
  HalFile input = openInput();
  ShortWriteSink output(63);

  EXPECT_FALSE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, 2, 1, false));
}

TEST(JpegConverterSafetyTest, BatchKeepsTheIndependentOutputWhenOneHeaderWriteFails) {
  HalFile input = openInput();
  ShortWriteSink failedOutput(1);
  ByteSink goodOutput;
  const JpegToBmpConverter::OneBitBmpTarget targets[] = {
      {&failedOutput, 2, 1, false},
      {&goodOutput, 2, 1, false},
  };

  EXPECT_EQ(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSize(input, targets, 2), 0b10U);
  EXPECT_EQ(goodOutput.bytes.size(), 66U);
}

}  // namespace
