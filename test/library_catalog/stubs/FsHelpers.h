#pragma once

#include <string>

namespace FsHelpers {
inline bool endsWith(const std::string& path, const char* suffix) {
  const std::string value(suffix);
  return path.size() >= value.size() && path.compare(path.size() - value.size(), value.size(), value) == 0;
}
inline bool hasEpubExtension(const std::string& path) { return endsWith(path, ".epub"); }
inline bool hasTxtExtension(const std::string& path) { return endsWith(path, ".txt"); }
inline bool hasMarkdownExtension(const std::string& path) { return endsWith(path, ".md"); }
inline bool checkFileExtension(const std::string& path, const char* extension) { return endsWith(path, extension); }
}  // namespace FsHelpers
