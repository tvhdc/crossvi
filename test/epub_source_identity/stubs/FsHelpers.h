#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace FsHelpers {
inline std::string normalisePath(const std::string& path) {
  std::vector<std::string_view> components;
  size_t start = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    if (i > start) {
      const std::string_view component(path.data() + start, i - start);
      if (component == "..") {
        if (!components.empty()) components.pop_back();
      } else if (component != ".") {
        components.push_back(component);
      }
    }
    start = i + 1;
  }

  std::string result;
  for (const std::string_view component : components) {
    if (!result.empty()) result += '/';
    result.append(component.data(), component.size());
  }
  return result;
}
inline std::string decodeUriEscapes(const std::string& path) { return path; }
inline bool hasCssExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".css";
}
inline bool hasPngExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".png";
}
inline bool hasJpgExtension(const std::string_view path) {
  return (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") ||
         (path.size() >= 5 && path.substr(path.size() - 5) == ".jpeg");
}
inline bool hasTxtExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".txt";
}
inline bool hasMarkdownExtension(const std::string_view path) {
  return path.size() >= 3 && path.substr(path.size() - 3) == ".md";
}
inline bool hasBmpExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".bmp";
}
}  // namespace FsHelpers
