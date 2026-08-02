#include <gtest/gtest.h>

#include "network/JsonBodyBuffer.h"

TEST(JsonBodyBuffer, RejectsFormBodiesBeforeRawAccess) {
  EXPECT_FALSE(JsonBodyBuffer::acceptsRawContentType("multipart/form-data; boundary=test"));
  EXPECT_FALSE(JsonBodyBuffer::acceptsRawContentType("Multipart/Form-Data; boundary=test"));
  EXPECT_FALSE(JsonBodyBuffer::acceptsRawContentType("application/x-www-form-urlencoded"));
  EXPECT_TRUE(JsonBodyBuffer::acceptsRawContentType("application/json"));
  EXPECT_TRUE(JsonBodyBuffer::acceptsRawContentType("text/plain"));
  EXPECT_TRUE(JsonBodyBuffer::acceptsRawContentType(""));
}

TEST(JsonBodyBuffer, AcceptsUploadsOnlyWhenTheFrameworkCreatesAnUploadObject) {
  EXPECT_TRUE(JsonBodyBuffer::acceptsMultipartUploadContentType("multipart/form-data; boundary=test"));
  EXPECT_FALSE(JsonBodyBuffer::acceptsMultipartUploadContentType("Multipart/Form-Data; boundary=test"));
  EXPECT_FALSE(JsonBodyBuffer::acceptsMultipartUploadContentType("application/json"));
  EXPECT_FALSE(JsonBodyBuffer::acceptsMultipartUploadContentType(""));
}

TEST(JsonBodyBuffer, AccumulatesBoundedJsonAndResetsAfterTake) {
  JsonBodyBuffer::State state;
  constexpr size_t limit = 8;
  const uint8_t first[] = {'{', '"', 'x'};
  const uint8_t second[] = {'"', ':', '1', '}'};

  state.start(limit);
  state.write(first, sizeof(first), limit);
  state.write(second, sizeof(second), limit);
  state.finish();

  auto completed = state.take();
  ASSERT_EQ(completed.error, JsonBodyBuffer::Error::None);
  ASSERT_TRUE(completed.complete);
  ASSERT_NE(completed.data, nullptr);
  EXPECT_STREQ(reinterpret_cast<const char*>(completed.data.get()), "{\"x\":1}");
  EXPECT_EQ(state.error, JsonBodyBuffer::Error::None);
  EXPECT_FALSE(state.complete);
  EXPECT_EQ(state.size, 0U);
  EXPECT_EQ(state.data, nullptr);
}

TEST(JsonBodyBuffer, MarksOversizeAndUnsupportedRequestsWithoutRetainingData) {
  JsonBodyBuffer::State state;
  constexpr size_t limit = 4;
  const uint8_t bytes[] = {'1', '2', '3', '4', '5'};

  state.start(limit);
  state.write(bytes, sizeof(bytes), limit);
  EXPECT_EQ(state.error, JsonBodyBuffer::Error::TooLarge);
  EXPECT_EQ(state.data, nullptr);

  state.rejectContentType();
  EXPECT_EQ(state.error, JsonBodyBuffer::Error::UnsupportedContentType);
  EXPECT_EQ(state.data, nullptr);
}
