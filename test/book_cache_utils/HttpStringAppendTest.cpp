#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" char* __wrap_http_utils_append_string(char** str, const char* newStr, int len);
extern "C" void* __real_realloc(void* ptr, size_t size);

namespace {
std::atomic<bool> failNextRealloc{false};
}

extern "C" void* __wrap_realloc(void* ptr, size_t size) {
  if (failNextRealloc.exchange(false)) return nullptr;
  return __real_realloc(ptr, size);
}

TEST(HttpStringAppend, BuildsFragmentedHeaderWithoutLosingTermination) {
  char* value = nullptr;
  ASSERT_NE(__wrap_http_utils_append_string(&value, "release-", -1), nullptr);
  ASSERT_NE(__wrap_http_utils_append_string(&value, "assets", 6), nullptr);
  ASSERT_NE(__wrap_http_utils_append_string(&value, ".githubusercontent.com", -1), nullptr);
  EXPECT_STREQ(value, "release-assets.githubusercontent.com");
  std::free(value);
}

TEST(HttpStringAppend, RejectsInvalidDestinationWithoutAllocation) {
  EXPECT_EQ(__wrap_http_utils_append_string(nullptr, "header", -1), nullptr);
}

TEST(HttpStringAppend, AllocationFailureKeepsOriginalStringValid) {
  char* value = static_cast<char*>(std::malloc(7));
  ASSERT_NE(value, nullptr);
  std::memcpy(value, "stable", 7);
  char* const original = value;

  failNextRealloc = true;
  EXPECT_EQ(__wrap_http_utils_append_string(&value, " redirect", -1), nullptr);
  EXPECT_EQ(value, original);
  EXPECT_STREQ(value, "stable");
  std::free(value);
}
