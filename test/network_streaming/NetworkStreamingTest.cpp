#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "network/HttpFileStreamer.h"

namespace {
class FakeFile {
 public:
  explicit FakeFile(std::vector<uint8_t> bytes, const size_t declaredSize = 0)
      : bytes_(std::move(bytes)), declaredSize_(declaredSize == 0 ? bytes_.size() : declaredSize) {}

  size_t size() const { return declaredSize_; }

  int read(void* destination, const size_t length) {
    const size_t available = bytes_.size() - position_;
    const size_t count = std::min(length, available);
    if (count == 0) return 0;
    std::memcpy(destination, bytes_.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }

 private:
  std::vector<uint8_t> bytes_;
  size_t declaredSize_ = 0;
  size_t position_ = 0;
};

class FakeClient {
 public:
  bool connected() const { return connected_; }

  size_t write(const uint8_t* data, const size_t length) {
    if (zeroWrite_) return 0;
    const size_t count = std::min(length, maxWrite_);
    output_.insert(output_.end(), data, data + count);
    if (disconnectAfter_ != 0 && output_.size() >= disconnectAfter_) connected_ = false;
    return count;
  }

  bool connected_ = true;
  bool zeroWrite_ = false;
  size_t maxWrite_ = SIZE_MAX;
  size_t disconnectAfter_ = 0;
  std::vector<uint8_t> output_;
};

TEST(NetworkStreaming, SendsTheWholeFileAcrossPartialWrites) {
  const std::vector<uint8_t> source{0, 1, 2, 3, 4, 5, 6};
  FakeFile file(source);
  FakeClient client;
  client.maxWrite_ = 2;
  uint8_t buffer[4];
  size_t serviceCalls = 0;

  const auto result = streamHttpFile(file, client, buffer, sizeof(buffer), [&] { ++serviceCalls; });

  EXPECT_TRUE(result.complete());
  EXPECT_EQ(result.bytesSent, source.size());
  EXPECT_EQ(client.output_, source);
  EXPECT_GT(serviceCalls, 1U);
}

TEST(NetworkStreaming, StopsWhenTheClientDisconnects) {
  FakeFile file({0, 1, 2, 3, 4, 5});
  FakeClient client;
  client.maxWrite_ = 2;
  client.disconnectAfter_ = 2;
  uint8_t buffer[4];

  const auto result = streamHttpFile(file, client, buffer, sizeof(buffer), [] {});

  EXPECT_FALSE(result.complete());
  EXPECT_EQ(result.bytesSent, 2U);
}

TEST(NetworkStreaming, StopsOnZeroWrite) {
  FakeFile file({0, 1, 2});
  FakeClient client;
  client.zeroWrite_ = true;
  uint8_t buffer[4];

  const auto result = streamHttpFile(file, client, buffer, sizeof(buffer), [] {});

  EXPECT_FALSE(result.complete());
  EXPECT_EQ(result.bytesSent, 0U);
}

TEST(NetworkStreaming, DetectsShortInput) {
  FakeFile file({0, 1}, 4);
  FakeClient client;
  uint8_t buffer[4];

  const auto result = streamHttpFile(file, client, buffer, sizeof(buffer), [] {});

  EXPECT_FALSE(result.complete());
  EXPECT_EQ(result.bytesSent, 2U);
  EXPECT_EQ(result.expectedBytes, 4U);
}
}  // namespace
