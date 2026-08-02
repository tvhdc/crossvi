#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

class Epub {
 public:
  struct SpineItem {
    std::string href;
  };

  explicit Epub(std::string contents) : contents_(std::move(contents)) {}

  int getSpineItemsCount() const { return 1; }
  SpineItem getSpineItem(int) const { return {"chapter.xhtml"}; }

  bool readItemContentsToStream(const std::string& href, Print& output, const size_t chunkSize) const {
    if (href != "chapter.xhtml" || chunkSize == 0) return false;
    for (size_t offset = 0; offset < contents_.size(); offset += chunkSize) {
      const size_t count = std::min(chunkSize, contents_.size() - offset);
      if (output.write(reinterpret_cast<const uint8_t*>(contents_.data() + offset), count) != count) return false;
    }
    return true;
  }

 private:
  std::string contents_;
};
