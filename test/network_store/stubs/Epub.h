#pragma once

#include <string>

class Epub {
 public:
  Epub(const std::string&, const std::string&) {}
  bool load(bool = true, bool = false) { return false; }
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};
