#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace JsonBodyBuffer {

enum class Error : uint8_t { None, Allocation, TooLarge, Aborted, UnsupportedContentType };

inline bool startsWithIgnoreCase(const std::string_view value, const std::string_view prefix) {
  if (value.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    const char left = value[i] >= 'A' && value[i] <= 'Z' ? value[i] + ('a' - 'A') : value[i];
    const char right = prefix[i] >= 'A' && prefix[i] <= 'Z' ? prefix[i] + ('a' - 'A') : prefix[i];
    if (left != right) return false;
  }
  return true;
}

inline bool acceptsRawContentType(const std::string_view contentType) {
  return !startsWithIgnoreCase(contentType, "multipart/") &&
         !startsWithIgnoreCase(contentType, "application/x-www-form-urlencoded");
}

inline bool acceptsMultipartUploadContentType(const std::string_view contentType) {
  // Match the framework parser's case-sensitive multipart/ check. A differently
  // cased value is routed through the raw callback and has no HTTPUpload object.
  constexpr std::string_view prefix = "multipart/";
  return contentType.size() >= prefix.size() && contentType.substr(0, prefix.size()) == prefix;
}

struct State {
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
  Error error = Error::None;
  bool complete = false;

  void start(const size_t limit) {
    *this = {};
    data.reset(new (std::nothrow) uint8_t[limit + 1]);
    if (!data) error = Error::Allocation;
  }

  void write(const uint8_t* source, const size_t length, const size_t limit) {
    if (error != Error::None) return;
    if (size > limit || length > limit - size) {
      data.reset();
      error = Error::TooLarge;
      return;
    }
    std::memcpy(&data[size], source, length);
    size += length;
  }

  void finish() {
    if (error == Error::None) data[size] = '\0';
    complete = true;
  }

  void abort() {
    data.reset();
    error = Error::Aborted;
  }

  void rejectContentType() {
    *this = {};
    error = Error::UnsupportedContentType;
  }

  State take() {
    State result = std::move(*this);
    *this = {};
    return result;
  }
};

}  // namespace JsonBodyBuffer
