#include <BoundedFileReader.h>
#include <BufferedFile.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
namespace fs = std::filesystem;

class SectionBufferedIoTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("crossvi-section-buffer-" + std::to_string(getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_);
    ASSERT_EQ(setenv("CROSSVI_SIM_SD", root_.c_str(), 1), 0);
    ASSERT_TRUE(Storage.begin());
  }

  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
};

TEST_F(SectionBufferedIoTest, PreservesPageBoundariesAcrossBufferedWritesAndReads) {
  constexpr uint32_t prefix = 0x12345678U;
  constexpr uint16_t suffix = 0xBEEFU;
  std::array<uint8_t, 700> payload{};
  for (size_t index = 0; index < payload.size(); ++index) payload[index] = static_cast<uint8_t>(index);

  HalFile output = Storage.open("/section.bin", O_RDWR | O_CREAT | O_TRUNC);
  ASSERT_TRUE(output);
  std::array<uint8_t, 64> writeScratch{};
  serialization::BufferedFileWriter writer(output, writeScratch.data(), writeScratch.size());
  serialization::writePod(writer, prefix);
  writer.write(payload.data(), payload.size());
  ASSERT_TRUE(writer.flush());
  ASSERT_EQ(output.write(&suffix, sizeof(suffix)), sizeof(suffix));
  ASSERT_TRUE(output.close());

  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("TST", "/section.bin", input));
  std::array<uint8_t, 512> readScratch{};
  BoundedFileReader reader(input, 0, sizeof(prefix) + payload.size(), readScratch.data(), readScratch.size());
  uint32_t decodedPrefix = 0;
  std::array<uint8_t, 700> decodedPayload{};
  EXPECT_TRUE(reader.readPod(decodedPrefix));
  EXPECT_EQ(decodedPrefix, prefix);
  EXPECT_TRUE(reader.readBytes(decodedPayload.data(), decodedPayload.size()));
  EXPECT_EQ(decodedPayload, payload);
  EXPECT_TRUE(reader.atEnd());
  uint8_t outsidePage = 0;
  EXPECT_FALSE(reader.readBytes(&outsidePage, sizeof(outsidePage)));
  EXPECT_EQ(outsidePage, 0);
  EXPECT_TRUE(input.close());
}

TEST_F(SectionBufferedIoTest, SkipUsesTheLogicalBufferedPosition) {
  std::array<uint8_t, 600> bytes{};
  for (size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<uint8_t>(index);
  HalFile output = Storage.open("/section.bin", O_RDWR | O_CREAT | O_TRUNC);
  ASSERT_TRUE(output);
  ASSERT_EQ(output.write(bytes.data(), bytes.size()), bytes.size());
  ASSERT_TRUE(output.close());

  HalFile input;
  ASSERT_TRUE(Storage.openFileForRead("TST", "/section.bin", input));
  BoundedFileReader reader(input, 10, 590);
  uint8_t value = 0;
  ASSERT_TRUE(reader.readBytes(&value, 1));
  EXPECT_EQ(value, 10);
  ASSERT_TRUE(reader.skip(500));
  ASSERT_TRUE(reader.readBytes(&value, 1));
  EXPECT_EQ(value, static_cast<uint8_t>(511));
  EXPECT_TRUE(input.close());
}

}  // namespace
