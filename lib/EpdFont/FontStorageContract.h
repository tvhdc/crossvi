#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FontStorageContract {

// The selected family is persisted in a 32-byte settings field. The path
// capacity covers /.fonts, a hidden family transaction directory, and the
// longest accepted .cpfont filename without truncation.
inline constexpr size_t MAX_FAMILY_NAME_BYTES = 31;
inline constexpr size_t MAX_CPFONT_FILENAME_BYTES = 96;
inline constexpr size_t FONT_PATH_CAPACITY = 160;

inline bool hasValidUtf8NameBytes(const char* name, const size_t length) {
  if (!name || length == 0 || name[0] == ' ' || name[length - 1] == ' ') return false;
  for (size_t offset = 0; offset < length;) {
    const uint8_t first = static_cast<uint8_t>(name[offset]);
    if (first <= 0x7F) {
      const bool allowed = (first >= '0' && first <= '9') || (first >= 'A' && first <= 'Z') ||
                           (first >= 'a' && first <= 'z') || first == ' ' || first == '-' || first == '_';
      if (!allowed) return false;
      ++offset;
      continue;
    }

    size_t continuationCount = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codepoint = first & 0x1FU;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codepoint = first & 0x0FU;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codepoint = first & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (continuationCount > length - offset - 1) return false;
    for (size_t index = 1; index <= continuationCount; ++index) {
      const uint8_t continuation = static_cast<uint8_t>(name[offset + index]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    offset += continuationCount + 1;
  }
  return true;
}

inline bool isValidFamilyName(const char* name) {
  if (!name || name[0] == '\0') return false;
  const size_t length = strlen(name);
  if (length > MAX_FAMILY_NAME_BYTES || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return false;
  return hasValidUtf8NameBytes(name, length);
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
  return hasValidUtf8NameBytes(name, length - extensionLength);
}

}  // namespace FontStorageContract
