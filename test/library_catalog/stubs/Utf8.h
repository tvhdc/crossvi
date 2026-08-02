#pragma once

#include <cstddef>
#include <cstring>

inline size_t utf8SafeTruncateBuffer(const char* text, const size_t maximum) {
  if (!text) return 0;
  const size_t length = std::strlen(text);
  if (length <= maximum) return length;
  size_t lead = maximum;
  while (lead > 0 && (static_cast<unsigned char>(text[lead]) & 0xC0U) == 0x80U) --lead;
  return lead;
}
