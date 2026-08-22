#include "GfxRenderer.h"

#include <BidiUtils.h>
#include <BuildScratch.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <limits>
#include <string_view>

#include "FontCacheManager.h"
#include "SmallCaps.h"

namespace {

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}

int32_t scaleSmallCapsAdvance(const int32_t value) { return value >= 0 ? (value * 3 + 2) / 4 : (value * 3 - 2) / 4; }

int scaleSmallCapsMetric(const int value) { return value * 3 / 4; }
int smallCapsExtent(const int value) { return (value * 3 + 3) / 4; }

int smallCapsSourceEnd(const int destination, const int sourceLimit) {
  const int sourceStart = destination * 4 / 3;
  return std::min(sourceLimit, std::max(sourceStart + 1, ((destination + 1) * 4 + 2) / 3));
}

const char* gfxRefreshModeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::FAST_REFRESH:
    default:
      return "FAST";
  }
}

void draw2BitFontPixel(const GfxRenderer& renderer, const GfxRenderer::RenderMode mode, const int x, const int y,
                       const uint8_t raw, const bool state) {
  const uint8_t value = 3 - raw;
  if (mode == GfxRenderer::BW && value < 3) {
    renderer.drawPixel(x, y, state);
  } else if (mode == GfxRenderer::GRAYSCALE_MSB && (value == 1 || value == 2)) {
    renderer.drawPixel(x, y, false);
  } else if (mode == GfxRenderer::GRAYSCALE_LSB && value == 1) {
    renderer.drawPixel(x, y, false);
  }
}
}  // namespace

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, BidiUtils::BidiBaseDir baseDir);

// Appends the shaped visual form of every RTL token in `text` to `shapedOut`.
// getTextAdvanceX() measures the bidi-reordered, Arabic-shaped codepoint stream,
// so the SD advance table must be warmed with the presentation forms as well as
// the logical codepoints — otherwise every RTL word measurement misses the fast
// path and falls through to onGlyphMiss(), which opens the .cpfont and reads
// glyph metadata + bitmap into the 8-slot overflow ring, once per glyph.
// Tokens without RTL lead bytes (0xD6-0xDB) are skipped with a byte scan, so
// pure-LTR text pays almost nothing.
void appendShapedRtlTokens(const char* text, std::string& shapedOut) {
  const auto isBreak = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  std::string token;
  std::string visual;
  const char* p = text;
  while (*p) {
    while (*p && isBreak(*p)) ++p;
    const char* start = p;
    bool hasRtlBytes = false;
    while (*p && !isBreak(*p)) {
      const auto b = static_cast<unsigned char>(*p);
      hasRtlBytes = hasRtlBytes || (b >= 0xD6 && b <= 0xDB);
      ++p;
    }
    if (!hasRtlBytes) continue;
    token.assign(start, p - start);
    if (BidiUtils::applyBidiVisual(token.c_str(), visual, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
      shapedOut += visual;
    }
  }
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    std::string shaped;
    appendShapedRtlTokens(utf8Text, shaped);
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

bool GfxRenderer::hasSdCardAdvanceTable(const int fontId) const {
  const auto it = sdCardFonts_.find(fontId);
  return it != sdCardFonts_.end() && it->second->hasAdvanceTable();
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    std::string shaped;
    for (const auto& w : words) {
      appendShapedRtlTokens(w.c_str(), shaped);
    }
    int missed =
        it->second->buildAdvanceTable(words, includeHyphen, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
}

void GfxRenderer::releaseFrameBufferForBuild() {
  // Lend the framebuffer's bytes IN PLACE: the allocation is never freed, so
  // it cannot move and repeated loans cannot fragment the heap (the previous
  // free+realloc model measurably decayed the max contiguous block over a
  // session). The bytes are deposited in the build-scratch registry so
  // memory-hungry build phases (e.g. InflateStream's tinfl state + window)
  // can claim them instead of allocating.
  uint32_t size = 0;
  uint8_t* scratch = display.lendFrameBufferStorage(&size);
  frameBuffer = nullptr;
  if (scratch) {
    buildscratch::lend(scratch, size);
  }
}

bool GfxRenderer::restoreFrameBufferAfterBuild() {
  buildscratch::reclaim();
  display.returnFrameBufferStorage();  // cannot fail: the allocation was never freed
  frameBuffer = display.getFrameBuffer();
  return frameBuffer != nullptr;
}

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer) {
  // Nesting guard: if the framebuffer is already lent out (an outer loan),
  // stay inert so this end() cannot return storage the outer loan still owns.
  if (!renderer_.hasFrameBuffer()) return;
  renderer_.releaseFrameBufferForBuild();
  active_ = true;
}

void GfxRenderer::FrameBufferLoan::end() {
  if (!active_) return;
  active_ = false;
  if (!renderer_.restoreFrameBufferAfterBuild()) {
    // Only reachable if the framebuffer never existed, which begin() already
    // asserts against; kept as a backstop since running blind helps nobody.
    LOG_ERR("GFX", "Framebuffer restore failed - restarting");
    ESP.restart();
  }
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

// Output of screenRectToAlignedMemRect: a rectangle in panel-memory
// coordinates whose x and width are guaranteed to be multiples of 8 (the
// SDK's EInkDisplay::displayWindow alignment requirement). `valid == false`
// means the input was empty or fully outside the panel.
struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

// Translate a screen-coordinate rectangle (the coordinate system used by
// fillRect / drawText / the rest of the renderer's public API) into a
// panel-memory rectangle suitable for direct framebuffer indexing. Rotates
// the rectangle's two opposite corners with rotateCoordinates(), takes the
// bounding box (which naturally swaps width/height in Portrait /
// PortraitInverted), then snaps the x extent outward to multiples of 8 and
// clamps to panel bounds. Precondition: panel dims are multiples of 8 (true
// for the 800x480 panel), so clamping cannot re-break alignment.
static AlignedMemRect screenRectToAlignedMemRect(GfxRenderer::Orientation orientation, int sx, int sy, int sw, int sh,
                                                 uint16_t panelWidth, uint16_t panelHeight) {
  AlignedMemRect out;
  if (sw <= 0 || sh <= 0) return out;

  int x0, y0, x1, y1;
  rotateCoordinates(orientation, sx, sy, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, sx + sw - 1, sy + sh - 1, &x1, &y1, panelWidth, panelHeight);

  const int memXLo = std::min(x0, x1);
  const int memYLo = std::min(y0, y1);
  const int memXHi = std::max(x0, x1) + 1;  // exclusive upper bound
  const int memYHi = std::max(y0, y1) + 1;

  // Snap x outward to multiples of 8.
  int alignedXLo = memXLo & ~0x7;        // round down
  int alignedXHi = (memXHi + 7) & ~0x7;  // round up

  if (alignedXLo < 0) alignedXLo = 0;
  if (alignedXHi > panelWidth) alignedXHi = panelWidth;
  int clampedYLo = memYLo;
  int clampedYHi = memYHi;
  if (clampedYLo < 0) clampedYLo = 0;
  if (clampedYHi > panelHeight) clampedYHi = panelHeight;

  if (alignedXHi <= alignedXLo || clampedYHi <= clampedYLo) return out;

  out.x = static_cast<uint16_t>(alignedXLo);
  out.y = static_cast<uint16_t>(clampedYLo);
  out.w = static_cast<uint16_t>(alignedXHi - alignedXLo);
  out.h = static_cast<uint16_t>(clampedYHi - clampedYLo);
  out.valid = true;
  return out;
}

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph at 50% scale. Used for SUP/SUB style bits.
//
// Each destination pixel represents a 2x2 source block. Drawing when that block
// contains ink preserves thin strokes that nearest-neighbor sampling can skip.
//
// The advance width is also halved in drawText() so layout reserves exactly the right
// horizontal space for the scaled glyph.
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  // Scale the glyph bearing by the same factor so the scaled glyph sits at the correct
  // pixel offset from the (already-shifted) cursor position.
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = cursorY - glyph->top / 2;

  if (fontData->is2Bit) {
    // 2-bit packed format: 4 pixels per byte, MSB first, 2 bits per pixel.
    // raw value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        uint8_t coverage = 0;
        uint8_t maxRaw = 0;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 2];
            const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
            coverage += raw;
            if (raw > maxRaw) maxRaw = raw;
          }
        }
        if (maxRaw >= 2 || coverage >= 2) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  } else {
    // 1-bit packed format: 8 pixels per byte, MSB first.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 3];
            const uint8_t bit = 7 - (pos & 7);
            if ((byte >> bit) & 1) {
              hasInk = true;
            }
          }
        }
        if (hasInk) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  }
}

static void renderCharSmallCaps(const GfxRenderer& renderer, const GfxRenderer::RenderMode renderMode,
                                const EpdFontFamily& fontFamily, const uint32_t cp, const int cursorX,
                                const int cursorY, const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;
  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int sourceWidth = glyph->width;
  const int sourceHeight = glyph->height;
  const int width = smallCapsExtent(sourceWidth);
  const int height = smallCapsExtent(sourceHeight);
  const int baseX = cursorX + scaleSmallCapsMetric(glyph->left);
  const int baseY = cursorY - scaleSmallCapsMetric(glyph->top);

  for (int y = 0; y < height; ++y) {
    const int sourceY = y * 4 / 3;
    const int sourceYEnd = smallCapsSourceEnd(y, sourceHeight);
    for (int x = 0; x < width; ++x) {
      const int sourceX = x * 4 / 3;
      const int sourceXEnd = smallCapsSourceEnd(x, sourceWidth);
      if (fontData->is2Bit) {
        uint8_t darkest = 0;
        for (int sy = sourceY; sy < sourceYEnd; ++sy) {
          for (int sx = sourceX; sx < sourceXEnd; ++sx) {
            const int position = sy * sourceWidth + sx;
            const uint8_t raw = (bitmap[position >> 2] >> ((3 - (position & 3)) * 2)) & 0x03;
            darkest = std::max(darkest, raw);
          }
        }
        draw2BitFontPixel(renderer, renderMode, baseX + x, baseY + y, darkest, pixelState);
      } else {
        bool ink = false;
        for (int sy = sourceY; sy < sourceYEnd && !ink; ++sy) {
          for (int sx = sourceX; sx < sourceXEnd; ++sx) {
            const int position = sy * sourceWidth + sx;
            if ((bitmap[position >> 3] >> (7 - (position & 7))) & 1U) {
              ink = true;
              break;
            }
          }
        }
        if (ink) renderer.drawPixel(baseX + x, baseY + y, pixelState);
      }
    }
  }
}

template <TextRotation rotation = TextRotation::None>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    // EpdFont already tried U+FFFD. Silently skip when even the replacement
    // glyph is absent; logging here runs once per render pass/pixel band and
    // can flood serial output for an otherwise safely handled bad font.
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW) {
            if (bmpVal < 3) {
              // Black (also paints over the grays in BW mode)
              renderer.drawPixel(screenX, screenY, pixelState);
            }
          } else {
            if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
              // Light gray (also mark the MSB if it's going to be a dark gray too)
              // Dedicated X3 gray LUTs now provide proper 4-level gray on both devices
              // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
              renderer.drawPixel(screenX, screenY, false);
            } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
              // Dark gray
              renderer.drawPixel(screenX, screenY, false);
            }
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
bool GfxRenderer::isPixelBlack(const int x, const int y) const {
  int phyX = 0;
  int phyY = 0;
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) return false;

  const uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) return false;
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
  }
  if (!target) return false;

  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);
  return (target[byteIndex] & (1U << bitPosition)) == 0;
}

void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Tiled grayscale: redirect writes to the strip scratch and clip to the
  // current band. Single predictable branch on the hot per-pixel path.
  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) {
      return;  // pixel outside the band currently being rendered
    }
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  if ((style & EpdFontFamily::SMALL_CAPS) != 0) return getTextAdvanceX(fontId, renderedText, style);

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(renderedText, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  const int yPos = y + getFontAscenderSize(fontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(renderedText, fontId, style);
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  const char* textCursor = renderedText;
  uint32_t cp;
  uint32_t prevCp = 0;
  bool previousSmallCap = false;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                       combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    const bool smallCap = (style & EpdFontFamily::SMALL_CAPS) != 0 && isSyntheticSmallCapsLowercase(cp);
    if (smallCap) {
      cp = syntheticSmallCapsUppercase(cp);
    } else {
      cp = font.applyLigatures(cp, textCursor, style);
    }

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      int32_t kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      if (previousSmallCap || smallCap) kernFP = scaleSmallCapsAdvance(kernFP);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);  // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (isSupSub) {
      // Halve the advance so the cursor advances by the same amount the scaled glyph
      // actually occupies, keeping spacing correct without needing a separate smaller font.
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    } else if (smallCap) {
      prevAdvanceFP = scaleSmallCapsAdvance(prevAdvanceFP);
    }

    if (isSupSub) {
      // yPos already carries the vertical offset applied by TextBlock::render().
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    } else if (smallCap) {
      renderCharSmallCaps(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
    previousSmallCap = smallCap;
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    // Byte-level scan: skip BiDi when no RTL script lead bytes are present.
    // Hebrew UTF-8 lead bytes: 0xD6-0xD7; Arabic/Syriac: 0xD8-0xDB.
    // This covers all RTL content without false negatives and avoids triggering
    // the full UAX#9 algorithm for Latin-extended, em-dashes, accented text, etc.
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - 1 - i, y + i, state);
    drawLine(x + width - 1 - i, y + i, x + width - 1 - i, y + height - 1 - i, state);
    drawLine(x + width - 1 - i, y + height - 1 - i, x + i, y + height - 1 - i, state);
    drawLine(x + i, y + height - 1 - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (state) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray>(x, y, width, height);
      break;
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(screenW, x + width);
  const int ly1 = std::min(screenH, y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // The bounding rect in physical space is the rect we need to fill — rotation
  // is rigid (no shear/stretch) so the bbox of the two corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip Y range to the active band and redirect writes.
  uint8_t* target = getWriteTarget();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  // Bit/byte layout: MSB-first within a byte, so phyX → bit (7 - (phyX & 7)).
  // Head and tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer: 0 = black, 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Both patterns have period 2 in logical
    // (x, y), so per physical row we precompute one byte that represents the
    // pattern across an 8-pixel stretch — every full byte in the row uses
    // that same value.
    //
    // dlxPerPhyX / dlyPerPhyX: how logical (x, y) change as phyX increments
    // along a physical row. Derived from inverting rotateCoordinates.
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:
        dlxPerPhyX = 0;
        dlyPerPhyX = 1;
        break;
      case PortraitInverted:
        dlxPerPhyX = 0;
        dlyPerPhyX = -1;
        break;
      case LandscapeClockwise:
        dlxPerPhyX = -1;
        dlyPerPhyX = 0;
        break;
      case LandscapeCounterClockwise:
        dlxPerPhyX = 1;
        dlyPerPhyX = 0;
        break;
    }

    // The dither pattern has period 2 in logical space, and each orientation
    // maps py to logical coords with a fixed parity relationship. The
    // blackMask byte therefore repeats with period 2 in py. Precompute both
    // variants outside the row loop to eliminate the per-row switch + 8-bit
    // construction loop.
    uint8_t blackMasks[2];
    for (int parityIdx = 0; parityIdx < 2; ++parityIdx) {
      const int samplePy = phyY0 + parityIdx;
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - samplePy;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = samplePy;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - samplePy;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = samplePy;
          break;
      }
      uint8_t mask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) mask |= static_cast<uint8_t>(1u << (7 - b));
      }
      blackMasks[samplePy & 1] = mask;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      const uint8_t blackMask = blackMasks[py & 1];
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          // Period 2, so every full byte in this row is exactly whiteMask.
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int size) const {
  // Plot the icon pixel-by-pixel through drawPixel (which applies the orientation
  // transform) instead of the byte-aligned framebuffer blit. The blit snaps the
  // icon's position to 8px (one byte) along the rotated axis, which prevents it
  // from aligning with adjacent text; per-pixel plotting is pixel-precise.
  // Icons are square and 1bpp (MSB-first, bit==0 = ink). The (size-1-row, col)
  // mapping reproduces the Portrait orientation the blit produced; drawIcon is
  // only called by the UI themes, which all render in forced Portrait.
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

bool GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY, const bool allowUpscale) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return false;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    return drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight, allowUpscale);
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && (fitScale < 1.0f || allowUpscale)) {
    scale = fitScale;
    isScaled = fitScale != 1.0f;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return false;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return false;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    const int logicalY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (scale > 1.0f) {
      const int firstY = y + static_cast<int>(std::floor(logicalY * scale));
      const int endY = y + static_cast<int>(std::floor((logicalY + 1) * scale));
      const int clippedY = std::max(0, firstY);
      const int clippedBottom = std::min(getScreenHeight(), std::max(firstY + 1, endY));
      if (clippedY >= clippedBottom) continue;

      for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
        const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
        const bool drawBlack = renderMode == BW && val < 3;
        const bool drawWhite = (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) ||
                               (renderMode == GRAYSCALE_LSB && val == 1);
        if (!drawBlack && !drawWhite) continue;

        const int logicalX = bmpX - cropPixX;
        const int firstX = x + static_cast<int>(std::floor(logicalX * scale));
        const int endX = x + static_cast<int>(std::floor((logicalX + 1) * scale));
        const int clippedX = std::max(0, firstX);
        const int clippedRight = std::min(getScreenWidth(), std::max(firstX + 1, endX));
        if (clippedX < clippedRight) {
          fillRect(clippedX, clippedY, clippedRight - clippedX, clippedBottom - clippedY, drawBlack);
        }
      }
      continue;
    }

    int screenY = logicalY;
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += y;  // the offset should not be scaled
    if (screenY < 0 || screenY >= getScreenHeight()) continue;

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);
  return true;
}

bool GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight, const bool allowUpscale) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && (bitmap.getWidth() > maxWidth || allowUpscale)) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = scale != 1.0f;
  }
  if (maxHeight > 0 && (bitmap.getHeight() > maxHeight || allowUpscale)) {
    const float heightScale = static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight());
    scale = std::min(scale, heightScale);
    isScaled = scale != 1.0f;
  }

  // A cached cover is a 1-bit BMP. Read all packed rows once and sample the
  // output pixels directly. The old path performed one SD read per source row
  // and then plotted every source pixel, so a large uncropped cover could hold
  // the render lock long enough to look like a frozen X3. This path is used for
  // both native-size and downscaled covers; the row path below remains the
  // fallback for strip/grayscale rendering or an allocation/read failure.
  if (!_stripActive && frameBuffer) {
    const uint64_t packedBytes64 =
        static_cast<uint64_t>(bitmap.getRowBytes()) * static_cast<uint64_t>(bitmap.getHeight());
    if (packedBytes64 > 0 && packedBytes64 <= std::numeric_limits<size_t>::max()) {
      const size_t packedBytes = static_cast<size_t>(packedBytes64);
      auto* packedRows = static_cast<uint8_t*>(malloc(packedBytes));
      if (packedRows && bitmap.readPackedRows(packedRows, packedBytes) == BmpReaderError::Ok) {
        const int outputWidth = isScaled
                                    ? std::max(1, static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1)
                                    : bitmap.getWidth();
        const int outputHeight = isScaled
                                     ? std::max(1, static_cast<int>(std::floor((bitmap.getHeight() - 1) * scale)) + 1)
                                     : bitmap.getHeight();

        int phyXBase = 0;
        int phyYBase = 0;
        int phyXStepX = 0;
        int phyYStepX = 0;
        int phyXStepY = 0;
        int phyYStepY = 0;
        switch (orientation) {
          case Portrait:
            phyYBase = panelHeight - 1;
            phyXStepY = 1;
            phyYStepX = -1;
            break;
          case LandscapeClockwise:
            phyXBase = panelWidth - 1;
            phyYBase = panelHeight - 1;
            phyXStepX = -1;
            phyYStepY = -1;
            break;
          case PortraitInverted:
            phyXBase = panelWidth - 1;
            phyXStepY = -1;
            phyYStepX = 1;
            break;
          case LandscapeCounterClockwise:
            phyXStepX = 1;
            phyYStepY = 1;
            break;
        }

        const bool zeroIsInk = bitmap.paletteLuminance(0) < 192;
        const bool oneIsInk = bitmap.paletteLuminance(1) < 192;
        if (!isScaled) {
          const int sourceBytes = (bitmap.getWidth() + 7) / 8;
          for (int outputY = 0; outputY < outputHeight; ++outputY) {
            const int sourceY = bitmap.isTopDown() ? outputY : bitmap.getHeight() - 1 - outputY;
            const uint8_t* sourceRow = packedRows + static_cast<size_t>(sourceY) * bitmap.getRowBytes();
            const int logicalY = y + outputY;
            const int rowPhyXBase = phyXBase + logicalY * phyXStepY;
            const int rowPhyYBase = phyYBase + logicalY * phyYStepY;
            for (int sourceByteIndex = 0; sourceByteIndex < sourceBytes; ++sourceByteIndex) {
              const uint8_t sourceByte = sourceRow[sourceByteIndex];
              uint8_t inkBits = 0;
              if (zeroIsInk) inkBits |= static_cast<uint8_t>(~sourceByte);
              if (oneIsInk) inkBits |= sourceByte;
              const int firstOutputX = sourceByteIndex * 8;
              const int pixels = std::min(8, bitmap.getWidth() - firstOutputX);
              for (int bit = 0; bit < pixels; ++bit) {
                if ((inkBits & (1U << (7 - bit))) == 0) continue;
                const int phyX = rowPhyXBase + (x + firstOutputX + bit) * phyXStepX;
                const int phyY = rowPhyYBase + (x + firstOutputX + bit) * phyYStepX;
                if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) continue;
                const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
                frameBuffer[byteIndex] &= static_cast<uint8_t>(~(1U << (7 - (phyX % 8))));
              }
            }
          }
          free(packedRows);
          return true;
        }

        for (int outputY = 0; outputY < outputHeight; ++outputY) {
          const int sampledY =
              isScaled ? std::min(bitmap.getHeight() - 1, static_cast<int>(std::floor(outputY / scale))) : outputY;
          const int sourceY = bitmap.isTopDown() ? sampledY : bitmap.getHeight() - 1 - sampledY;
          const uint8_t* sourceRow = packedRows + static_cast<size_t>(sourceY) * bitmap.getRowBytes();
          const int logicalY = y + outputY;
          const int rowPhyXBase = phyXBase + logicalY * phyXStepY;
          const int rowPhyYBase = phyYBase + logicalY * phyYStepY;
          for (int outputX = 0; outputX < outputWidth; ++outputX) {
            const int sourceX =
                isScaled ? std::min(bitmap.getWidth() - 1, static_cast<int>(std::floor(outputX / scale))) : outputX;
            const uint8_t paletteIndex = (sourceRow[sourceX / 8] >> (7 - (sourceX % 8))) & 0x1;
            if (!(paletteIndex == 0 ? zeroIsInk : oneIsInk)) continue;
            const int phyX = rowPhyXBase + (x + outputX) * phyXStepX;
            const int phyY = rowPhyYBase + (x + outputX) * phyYStepX;
            if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) continue;
            const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
            frameBuffer[byteIndex] &= static_cast<uint8_t>(~(1U << (7 - (phyX % 8))));
          }
        }
        free(packedRows);
        return true;
      }
      free(packedRows);
      bitmap.rewindToData();
    }
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return false;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return false;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  free(outputRow);
  free(rowBytes);
  return true;
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX, nodeX + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  if (_stripActive) {
    // Clear only the active band's scratch, not the shared framebuffer.
    memset(_stripBuf, color, static_cast<size_t>(panelWidthBytes) * _stripRows);
    return;
  }
  display.clearScreen(color);
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}

void GfxRenderer::invertScreen() const {
  if (!frameBuffer) return;
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::invertRect(const int x, const int y, const int width, const int height) const {
  if (!frameBuffer || width <= 0 || height <= 0) return;

  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(getScreenWidth(), x + width);
  const int ly1 = std::min(getScreenHeight(), y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);
  const int phyY0 = std::min(paY, pbY);
  const int phyY1 = std::max(paY, pbY);
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));

  for (int py = phyY0; py <= phyY1; ++py) {
    uint8_t* row = frameBuffer + static_cast<int32_t>(py) * panelWidthBytes;
    if (byteStart == byteEnd) {
      row[byteStart] ^= headMask & tailMask;
      continue;
    }
    row[byteStart] ^= headMask;
    for (int byte = byteStart + 1; byte < byteEnd; ++byte) row[byte] ^= 0xFFu;
    row[byteEnd] ^= tailMask;
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  const bool actualTurnOff = turnOffScreen;
  LOG_DBG("GFX", "displayBuffer req=%s off=%u fadingFix=%u actual_off=%u", gfxRefreshModeName(refreshMode),
          static_cast<unsigned>(turnOffScreen), static_cast<unsigned>(fadingFix), static_cast<unsigned>(actualTurnOff));
  display.displayBuffer(refreshMode, actualTurnOff);
}

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  if (dst == nullptr || w <= 0 || h <= 0) return 0;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return 0;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8
  const size_t needed = rowBytes * mem.h;
  if (needed > dstCapacity) return 0;

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    uint8_t* dstRow = dst + (static_cast<size_t>(row) * rowBytes);
    memcpy(dstRow, srcRow, rowBytes);
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  if (src == nullptr || w <= 0 || h <= 0) return;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = src + (static_cast<size_t>(row) * rowBytes);
    uint8_t* dstRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    memcpy(dstRow, srcRow, rowBytes);
  }
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style, int* const finalWidth) const {
  if (!text || maxWidth <= 0) {
    if (finalWidth) *finalWidth = 0;
    return "";
  }

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    if (finalWidth) *finalWidth = textWidth;
    // Text fits, return as is
    return item;
  }

  item.reserve(item.size() + 3);
  item += ellipsis;
  while (true) {
    textWidth = getTextWidth(fontId, item.c_str(), style);
    if (textWidth < maxWidth || item.size() <= 3) break;
    item.resize(item.size() - 3);
    utf8RemoveLastChar(item);
    item += ellipsis;
  }

  if (finalWidth) *finalWidth = textWidth;
  return item;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string_view remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent;
      lastContent.reserve(currentLine.size() + (currentLine.empty() ? 0 : 1) + remaining.size());
      if (!currentLine.empty()) {
        lastContent = currentLine;
        lastContent.push_back(' ');
      }
      lastContent.append(remaining);
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word.assign(remaining);
      remaining = {};
    } else {
      word.assign(remaining.substr(0, spacePos));
      remaining.remove_prefix(spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // Measure the exact codepoint stream drawText renders: bidi-reordered and
  // Arabic-shaped (contextual presentation forms, Lam-Alef collapse).
  // Measuring the raw logical text counts the Alef a ligature absorbs and
  // uses base-letter advances instead of presentation-form advances, so RTL
  // lines come out wider than they draw — uneven word gaps and a ragged
  // right margin.
  std::string visual;
  text = resolveVisualText(text, visual, BidiUtils::BidiBaseDir::AUTO);

  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    const auto fontIt = fontMap.find(fontId);
    if (fontIt == fontMap.end()) {
      LOG_ERR("GFX", "Font %d not found", fontId);
      return 0;
    }
    const auto& font = fontIt->second;
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
      if (BidiUtils::isTransparentMark(cp)) {
        continue;
      }
      const bool smallCap = (style & EpdFontFamily::SMALL_CAPS) != 0 && isSyntheticSmallCapsLowercase(cp);
      if (smallCap) cp = syntheticSmallCapsUppercase(cp);
      int32_t advFP = sdIt->second->getAdvance(cp, styleIdx);
      if (advFP == 0 && !utf8IsCombiningMark(cp)) {
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        advFP = glyph ? glyph->advanceX : 0;
      }
      widthFP += isSupSub ? (advFP + 1) / 2 : (smallCap ? scaleSmallCapsAdvance(advFP) : advFP);
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  bool previousSmallCap = false;
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
    if (BidiUtils::isTransparentMark(cp)) {
      continue;
    }
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    const bool smallCap = (style & EpdFontFamily::SMALL_CAPS) != 0 && isSyntheticSmallCapsLowercase(cp);
    if (smallCap) {
      cp = syntheticSmallCapsUppercase(cp);
    } else {
      cp = font.applyLigatures(cp, text, style);
    }

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      int32_t kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      if (previousSmallCap || smallCap) kernFP = scaleSmallCapsAdvance(kernFP);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);  // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    } else if (smallCap) {
      prevAdvanceFP = scaleSmallCapsAdvance(prevAdvanceFP);
    }
    prevCp = cp;
    previousSmallCap = smallCap;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::anchorOverRotated90CW(anchor, lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback, const bool turnOffScreen) const {
  display.displayGrayscaleBase(fallback, turnOffScreen);
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen) const { display.displayGrayBuffer(turnOffScreen); }

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return display.supportsStripGrayscale(); }

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
