#pragma once

#include <cstddef>
#include <cstdint>

inline uint32_t utf8NextCodepoint(const unsigned char** input) {
  const unsigned char lead = **input;
  size_t length = 1;
  if ((lead & 0xE0U) == 0xC0U) length = 2;
  if ((lead & 0xF0U) == 0xE0U) length = 3;
  if ((lead & 0xF8U) == 0xF0U) length = 4;
  *input += length;
  return lead;
}
