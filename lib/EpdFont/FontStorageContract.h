#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>

namespace FontStorageContract {

// The selected family is persisted in a 32-byte settings field. The path
// capacity covers /.fonts, a hidden family transaction directory, and the
// longest accepted .cpfont filename without truncation.
inline constexpr size_t MAX_FAMILY_NAME_BYTES = 31;
inline constexpr size_t MAX_CPFONT_FILENAME_BYTES = 96;
inline constexpr size_t FONT_PATH_CAPACITY = 160;

inline bool isValidFamilyName(const char* name) {
  if (!name || name[0] == '\0') return false;
  const size_t length = strlen(name);
  if (length > MAX_FAMILY_NAME_BYTES || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return false;
  for (const char* cursor = name; *cursor; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (!std::isalnum(value) && value != '-' && value != '_') return false;
  }
  return true;
}

inline bool isValidCpfontFilename(const char* name) {
  if (!name || name[0] == '\0' || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return false;
  static constexpr char extension[] = ".cpfont";
  static constexpr size_t extensionLength = sizeof(extension) - 1;
  const size_t length = strlen(name);
  if (length <= extensionLength || length > MAX_CPFONT_FILENAME_BYTES ||
      strcmp(name + length - extensionLength, extension) != 0) {
    return false;
  }
  for (size_t i = 0; i < length - extensionLength; ++i) {
    const unsigned char value = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(value) && value != '-' && value != '_') return false;
  }
  return true;
}

}  // namespace FontStorageContract
