#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "SmallCaps.h"

namespace {

constexpr size_t MAX_SCAN_TEXT_BYTES_PER_STYLE = 2048;

size_t utf8EncodedSize(const uint32_t codepoint) {
  if (codepoint <= 0x7FU) return 1;
  if (codepoint <= 0x7FFU) return 2;
  if (codepoint <= 0xFFFFU) return 3;
  return 4;
}

void appendBoundedText(std::string& destination, const char* text) {
  if (!text || !*text || destination.size() >= MAX_SCAN_TEXT_BYTES_PER_STYLE) return;
  if (destination.capacity() < MAX_SCAN_TEXT_BYTES_PER_STYLE) destination.reserve(MAX_SCAN_TEXT_BYTES_PER_STYLE);

  const size_t textLength = strlen(text);
  size_t appendLength = std::min(textLength, MAX_SCAN_TEXT_BYTES_PER_STYLE - destination.size());
  if (appendLength < textLength) {
    while (appendLength > 0 && (static_cast<uint8_t>(text[appendLength]) & 0xC0U) == 0x80U) --appendLength;
  }
  destination.append(text, appendLength);
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releasePageCache() {
  if (fontDecompressor_) fontDecompressor_->releasePageCache();
  // SD fonts keep their existing page-scoped behavior. Only the new bounded
  // built-in glyph ring survives across page turns.
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::clearAllCaches() {
  clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearPersistentCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_) return;
  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = font->second.getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || !*text) return;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03U;
  std::string& scanText = scanTextByStyle_[baseStyle];
  if (scanText.capacity() < MAX_SCAN_TEXT_BYTES_PER_STYLE) scanText.reserve(MAX_SCAN_TEXT_BYTES_PER_STYLE);

  if ((style & EpdFontFamily::SMALL_CAPS) != 0) {
    const auto* cursor = reinterpret_cast<const uint8_t*>(text);
    while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
      const uint32_t renderedCp = isSyntheticSmallCapsLowercase(cp) ? syntheticSmallCapsUppercase(cp) : cp;
      if (utf8EncodedSize(renderedCp) > MAX_SCAN_TEXT_BYTES_PER_STYLE - scanText.size()) break;
      utf8AppendCodepoint(renderedCp, scanText);
    }
  } else {
    appendBoundedText(scanText, text);
  }
  if (scanFontId_ == 0) scanFontId_ = fontId;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->releasePageCache();
  manager_->resetStats();
  for (auto& text : manager_->scanTextByStyle_) text.clear();
  manager_->scanFontId_ = 0;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanFontId_ == 0) return;

  const auto sdFont = manager_->sdCardFonts_.find(manager_->scanFontId_);
  if (sdFont != manager_->sdCardFonts_.end()) {
    // Multiple requested styles can resolve to one physical SD-font style.
    // Merge only those fallback groups so a later prewarm does not replace
    // glyphs prepared by an earlier call for the same physical style.
    std::array<int8_t, 4> ownerByResolvedStyle = {-1, -1, -1, -1};
    for (uint8_t requestedStyle = 0; requestedStyle < 4; ++requestedStyle) {
      if (manager_->scanTextByStyle_[requestedStyle].empty()) continue;
      const uint8_t resolvedStyle = sdFont->second->resolveStyle(requestedStyle);
      int8_t& owner = ownerByResolvedStyle[resolvedStyle];
      if (owner < 0) {
        owner = static_cast<int8_t>(requestedStyle);
      } else {
        appendBoundedText(manager_->scanTextByStyle_[owner], manager_->scanTextByStyle_[requestedStyle].c_str());
      }
    }
    for (uint8_t resolvedStyle = 0; resolvedStyle < 4; ++resolvedStyle) {
      const int8_t owner = ownerByResolvedStyle[resolvedStyle];
      if (owner < 0) continue;
      manager_->prewarmCache(manager_->scanFontId_, manager_->scanTextByStyle_[owner].c_str(),
                             static_cast<uint8_t>(1U << resolvedStyle));
    }
  } else {
    for (uint8_t style = 0; style < 4; ++style) {
      if (manager_->scanTextByStyle_[style].empty()) continue;
      manager_->prewarmCache(manager_->scanFontId_, manager_->scanTextByStyle_[style].c_str(),
                             static_cast<uint8_t>(1U << style));
    }
  }
  for (auto& text : manager_->scanTextByStyle_) text.clear();
  manager_->scanFontId_ = 0;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scan text is empty)
    manager_->releasePageCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
