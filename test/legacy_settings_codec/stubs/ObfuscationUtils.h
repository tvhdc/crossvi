#pragma once

#include <WString.h>

#include <string>

namespace obfuscation {

inline String obfuscateToBase64(const std::string& value) { return String(value); }

inline std::string deobfuscateFromBase64(const char* value, bool* ok = nullptr) {
  if (ok) *ok = value != nullptr;
  return value ? value : "";
}

}  // namespace obfuscation
