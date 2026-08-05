#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <optional>

namespace PageSourceAnchor {

inline std::optional<uint32_t> first(const Page& page) {
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block) continue;
    for (uint16_t word = 0; word < block->wordCount(); ++word) {
      if (block->hasSourceAnchor(word)) return block->sourceStart(word);
    }
  }
  return std::nullopt;
}

inline bool contains(const Page& page, const uint32_t offset) {
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block) continue;
    for (uint16_t word = 0; word < block->wordCount(); ++word) {
      if (block->hasSourceAnchor(word) && block->sourceStart(word) <= offset && offset < block->sourceEnd(word)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace PageSourceAnchor
