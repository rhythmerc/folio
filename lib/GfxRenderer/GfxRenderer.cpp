#include "GfxRenderer.h"

#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>

#include "FontCacheManager.h"

namespace {

const char* resolveVisualText(const char* text, std::string& visualBuffer, int paragraphLevel);

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}
}  // namespace

std::map<int, EpdFontFamily>::const_iterator GfxRenderer::resolveFontIt(int fontId) const {
  return fontMap.find(fontId);
}

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
  // For SD card fonts every glyph is loaded on demand into the overflow cache.
  // getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for glyphs not in the overflow cache
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
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    int missed = it->second->buildAdvanceTable(words, includeHyphen, styleMask);
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
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
    return;
  }
  // If a glyph fallback is already wired and this isn't it, point the new family at it.
  // (When the fallback itself is registered later, setGlyphFallbackFont retro-wires.)
  if (glyphFallbackFontId_ != 0 && fontId != glyphFallbackFontId_) {
    const auto fbIt = fontMap.find(glyphFallbackFontId_);
    if (fbIt != fontMap.end()) {
      result.first->second.setFallback(fbIt->second.getRegular());
    }
  }
}

void GfxRenderer::setGlyphFallbackFont(const int fontId) {
  glyphFallbackFontId_ = fontId;
  if (fontId == 0) return;
  const auto it = fontMap.find(fontId);
  if (it == fontMap.end()) {
    LOG_ERR("GFX", "Glyph fallback font %d not registered", fontId);
    return;
  }
  const EpdFont* fallbackFont = it->second.getRegular();
  if (!fallbackFont) {
    LOG_ERR("GFX", "Glyph fallback font %d has no regular style", fontId);
    return;
  }
  // Retro-wire fallback into every other registered family. Each family's
  // setFallback propagates to all four style fonts (which are global EpdFont
  // instances shared across registrations), so this single pass covers every
  // EpdFont reachable through the renderer.
  for (auto& [id, family] : fontMap) {
    if (id == fontId) continue;
    family.setFallback(fallbackFont);
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

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
//
// Callers resolve the glyph + its owning fontData once (via EpdFontFamily::getGlyph(cp, style, &outData))
// and pass both in. This keeps measurement (drawText's first call) and rendering (this function)
// referencing the same EpdFontData — important when the glyph came from the system glyph-fallback
// font, in which case `fontData` is the FALLBACK family's data, not the primary's. Passing it in
// also avoids a second interval scan / SD-overflow lookup per character.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode, const EpdGlyph* glyph,
                           const EpdFontData* fontData, int cursorX, int cursorY, const bool pixelState) {
  if (!glyph || !fontData) return;

  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

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

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
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

  // Calculate byte position and bit position
  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdFontData* combiningData = nullptr;
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style, &combiningData);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, combiningGlyph->left,
                                                       combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, combiningGlyph, combiningData, combiningX, yPos - raiseBy,
                                         black);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    //
    // Kerning across the glyph-fallback boundary naturally degrades to 0: the
    // primary's kern matrix doesn't list codepoints that aren't in its coverage,
    // and the fallback's matrix isn't consulted from here. So even when prev/cur
    // straddle primary and fallback, the lookup returns 0 — no special handling
    // is needed to suppress cross-font pair kerning.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdFontData* glyphData = nullptr;
    const EpdGlyph* glyph = font.getGlyph(cp, style, &glyphData);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    renderCharImpl<TextRotation::None>(*this, renderMode, glyph, glyphData, lastBaseX, yPos, black);
    prevCp = cp;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
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

// Fast-path rectangle fill. Convention reminder: framebuffer bit 1 == white,
// bit 0 == black. `state=true` means "draw black" (matches drawPixel).
//
// The implementation lives in fillRectImpl<Color> — see the template below.
// For B/W, the inner loop collapses to head-mask + memset + tail-mask per
// physical row. For dither colors, the per-row byte pattern is recomputed
// once from inverse-rotated logical (x, y); within a row the dither has
// period 2, so the same byte pattern applies to every full byte in the row.
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

template <bool transparent>
void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black, transparent>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White, transparent>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray, transparent>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray, transparent>(x, y, width, height);
      break;
  }
}

void GfxRenderer::drawDitheredLine(const int x, const int y, const int length) const {
  if (length <= 0) return;

  // Clip the logical 1px row to the visible area.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  if (y < 0 || y >= screenH) return;
  const int lx0 = std::max(0, x);
  const int lx1 = std::min(screenW, x + length);  // exclusive
  if (lx0 >= lx1) return;

  // Rotate the first logical pixel once and derive the constant per-logical-x
  // physical step (rotation is rigid, so the step is ±1 on a single axis).
  int pX, pY, pXn, pYn;
  rotateCoordinates(orientation, lx0, y, &pX, &pY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx0 + 1, y, &pXn, &pYn, panelWidth, panelHeight);
  const int dPhyX = pXn - pX;
  const int dPhyY = pYn - pY;

  // Walk only the inked pixels (even logical x → period-2 half-tone, visible at
  // any y), stepping the physical position by 2x the per-pixel delta and writing
  // the framebuffer directly. Bit value 0 = ink (black).
  const int firstEven = (lx0 & 1) ? lx0 + 1 : lx0;
  int phyX = pX + (firstEven - lx0) * dPhyX;
  int phyY = pY + (firstEven - lx0) * dPhyY;
  const int stepX = dPhyX * 2;
  const int stepY = dPhyY * 2;
  for (int lx = firstEven; lx < lx1; lx += 2) {
    const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
    frameBuffer[byteIndex] &= ~(1u << (7 - (phyX % 8)));
    phyX += stepX;
    phyY += stepY;
  }
}

template <Color C, bool transparent>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  // Transparent paints only the dither's black dots, so a solid white fill has
  // nothing to draw — skip it entirely.
  if constexpr (transparent && C == Color::White) return;

  if (width <= 0 || height <= 0) return;

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
  const int phyY0 = std::min(paY, pbY);
  const int phyY1 = std::max(paY, pbY);

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
      uint8_t* row = frameBuffer + static_cast<int32_t>(py) * panelStride;
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

    for (int py = phyY0; py <= phyY1; ++py) {
      // Logical coords at bit 7 of the starting byte (phyX = byteStart * 8).
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - py;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = py;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - py;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = py;
          break;
      }

      // Build the row's "black bitmask" by walking the 8 bit positions and
      // applying the dither rule on the inverse-rotated logical coords.
      uint8_t blackMask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) blackMask |= static_cast<uint8_t>(1u << (7 - b));
      }
      [[maybe_unused]] const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = frameBuffer + static_cast<int32_t>(py) * panelStride;
      if constexpr (transparent) {
        // Transparent: paint only the pattern's black dots (clear bits to
        // 0 = black ink) within the rect; white-pattern pixels leave the
        // background untouched.
        if (byteStart == byteEnd) {
          const uint8_t rectMask = headMask & tailMask;
          row[byteStart] &= static_cast<uint8_t>(~(rectMask & blackMask));
        } else {
          row[byteStart] &= static_cast<uint8_t>(~(headMask & blackMask));
          // Period 2, so blackMask is the same for every full byte in this row.
          for (int i = byteStart + 1; i < byteEnd; ++i) {
            row[i] &= static_cast<uint8_t>(~blackMask);
          }
          row[byteEnd] &= static_cast<uint8_t>(~(tailMask & blackMask));
        }
      } else if (byteStart == byteEnd) {
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

// fillRectImpl is private and only invoked from fillRect / fillRectDither in
// this translation unit, so it is instantiated implicitly. fillRectDither is
// called from other TUs, so its two specializations are instantiated here.
template void GfxRenderer::fillRectDither<false>(int, int, int, int, Color) const;
template void GfxRenderer::fillRectDither<true>(int, int, int, int, Color) const;

void GfxRenderer::drawCircle(const int cx, const int cy, const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) return;
  // Midpoint circle algorithm — plots the 8-way symmetric outline.
  int x = radius;
  int y = 0;
  int d = 1 - radius;
  while (x >= y) {
    fillRectDither(cx + x, cy + y, 1, 1, color);
    fillRectDither(cx - x, cy + y, 1, 1, color);
    fillRectDither(cx + x, cy - y, 1, 1, color);
    fillRectDither(cx - x, cy - y, 1, 1, color);
    fillRectDither(cx + y, cy + x, 1, 1, color);
    fillRectDither(cx - y, cy + x, 1, 1, color);
    fillRectDither(cx + y, cy - x, 1, 1, color);
    fillRectDither(cx - y, cy - x, 1, 1, color);
    ++y;
    if (d <= 0) {
      d += 2 * y + 1;
    } else {
      --x;
      d += 2 * (y - x) + 1;
    }
  }
}

void GfxRenderer::fillCircle(const int cx, const int cy, const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) return;
  // Midpoint circle — fill horizontal spans for each scanline pair.
  int x = radius;
  int y = 0;
  int d = 1 - radius;
  while (x >= y) {
    fillRectDither(cx - x, cy + y, 2 * x + 1, 1, color);
    fillRectDither(cx - x, cy - y, 2 * x + 1, 1, color);
    fillRectDither(cx - y, cy + x, 2 * y + 1, 1, color);
    fillRectDither(cx - y, cy - x, 2 * y + 1, 1, color);
    ++y;
    if (d <= 0) {
      d += 2 * y + 1;
    } else {
      --x;
      d += 2 * (y - x) + 1;
    }
  }
}

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

  if (cornerRadius <= 0) {
    fillRectDither(x, y, width, height, color);
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

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  display.drawImageTransparent(bitmap, y, getScreenWidth() - width - x, height, width);
}

void GfxRenderer::drawIcon(const Bitmap1Bit& icon, const int x, const int y, const bool inverted) const {
  // Icon arrays are authored in physical orientation (for the legacy byte-blit),
  // but blit1Bit walks its source in logical orientation. Rotate 90° CW into
  // logical space so the glyph isn't drawn sideways. Square icons only.
  constexpr int kMaxIconDim = 32;
  if (icon.bitmap == nullptr || icon.width <= 0 || icon.height <= 0 || icon.width > kMaxIconDim ||
      icon.height > kMaxIconDim) {
    LOG_ERR("GFX", "drawIcon: unsupported icon %dx%d (max %d)", icon.width, icon.height, kMaxIconDim);
    return;
  }

  const int srcStride = (icon.width + 7) / 8;
  const int rotW = icon.height;  // 90° CW swaps dimensions
  const int rotH = icon.width;
  const int rotStride = (rotW + 7) / 8;
  uint8_t rotated[kMaxIconDim * ((kMaxIconDim + 7) / 8)] = {};

  // rot90CW: rotated(col=i, row=j) = src(col=j, row=height-1-i). Bit value is
  // copied verbatim (1 = background, 0 = ink), matching blit1Bit's convention.
  for (int i = 0; i < rotW; ++i) {
    const uint8_t* s = icon.bitmap + (icon.height - 1 - i) * srcStride;
    for (int j = 0; j < rotH; ++j) {
      if (s[j >> 3] & static_cast<uint8_t>(0x80u >> (j & 7))) {
        rotated[j * rotStride + (i >> 3)] |= static_cast<uint8_t>(0x80u >> (i & 7));
      }
    }
  }

  if (inverted) {
    blit1Bit<BlitMode::TransparentWhite>(rotated, rotStride, rotW, rotH, x, y, 0);
  } else {
    blit1Bit<BlitMode::TransparentBlack>(rotated, rotStride, rotW, rotH, x, y, 0);
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
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

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
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
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

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
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
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
}

// ============================================================================
// Image cache — caller-owned, fixed-capacity bitmap cache.
// ============================================================================
//
// Storage lives in a BitmapCacheManager owned by the caller (typically an
// activity that knows its working set, e.g. LibraryActivity's 9-tile shelf).
// The renderer just decodes BMPs into the cache on miss and rasterizes from
// the cached pixels on hit. When the working set rotates, the owner calls
// BitmapCacheManager::clear() before re-populating — no LRU eviction, no
// negative-result set, no global state that grows across paints.

bool GfxRenderer::decodeThumbInto(const char* path, uint8_t* dst, std::size_t dstCap, int& outW, int& outH,
                                  bool& outTopDown) {
  // Decode all rows of the 1-bit BMP at `path` into the caller's 2bpp buffer
  // (Bitmap::readNextRow format). No allocation: the prefetch worker hands us a
  // single reused scratch, so steady-state scrolling never churns the heap.
  outW = 0;
  outH = 0;
  outTopDown = false;
  if (path == nullptr || path[0] == '\0' || dst == nullptr) return false;

  FsFile file;
  if (!Storage.openFileForRead("GFX", path, file)) return false;

  // Bitmap currently requires non-dithered for 1-bit; covers + thumbs render at
  // threshold so BW mode is correct.
  Bitmap bitmap(file, /*dithering=*/false);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  if (!bitmap.is1Bit()) return false;

  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  if (width <= 0 || height <= 0) return false;

  const int stride = (width + 3) / 4;
  const size_t bufBytes = static_cast<size_t>(stride) * static_cast<size_t>(height);
  if (bufBytes == 0 || bufBytes > dstCap) {
    if (bufBytes > dstCap)
      LOG_ERR("GFX", "Thumb too large: %u > %u for %s", static_cast<unsigned>(bufBytes),
              static_cast<unsigned>(dstCap), path);
    return false;
  }

  // Stack row scratch for the file's native row size. Library thumbs are
  // ≤120 px wide (1-bit → 15 B, padded to 16); 64 B leaves comfortable margin.
  const size_t rowBytes = bitmap.getRowBytes();
  uint8_t rowScratch[64];
  if (rowBytes > sizeof(rowScratch)) {
    LOG_ERR("GFX", "Thumb row too wide: %u for %s", static_cast<unsigned>(rowBytes), path);
    return false;
  }

  for (int row = 0; row < height; ++row) {
    if (bitmap.readNextRow(dst + row * stride, rowScratch) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Thumb row %d read failed for %s", row, path);
      return false;
    }
  }

  outW = width;
  outH = height;
  outTopDown = bitmap.isTopDown();
  return true;
}

void GfxRenderer::computeThumbTarget(int srcW, int srcH, int maxW, int maxH, int& outW, int& outH) {
  float scale = 1.0f;
  if (maxW > 0 && srcW > maxW) scale = static_cast<float>(maxW) / static_cast<float>(srcW);
  if (maxH > 0 && srcH > maxH) scale = std::min(scale, static_cast<float>(maxH) / static_cast<float>(srcH));
  outW = static_cast<int>(srcW * scale);
  outH = static_cast<int>(srcH * scale);
}

bool GfxRenderer::peekCachedBitmapDimensions(BitmapCacheManager& cache, const char* path, int* outWidth,
                                             int* outHeight) const {
  if (path == nullptr || path[0] == '\0') return false;
  auto* entry = cache.get(path);
  if (entry == nullptr) return false;
  if (outWidth) *outWidth = entry->width;
  if (outHeight) *outHeight = entry->height;
  return true;
}

bool GfxRenderer::buildScaledFromSource(BitmapCacheManager::Entry& entry, const uint8_t* src2bpp, int srcW, int srcH,
                                        bool topDown, int targetW, int targetH, Arena& arena) const {
  if (src2bpp == nullptr || srcW <= 0 || srcH <= 0 || targetW <= 0 || targetH <= 0) return false;

  const int scaledStride = (targetW + 7) / 8;
  const size_t scaledBytes = static_cast<size_t>(scaledStride) * static_cast<size_t>(targetH);

  auto* raw = static_cast<uint8_t*>(arena.allocate(scaledBytes));
  if (raw == nullptr) {
    LOG_ERR("GFX", "Cover arena full: %u bytes", static_cast<unsigned>(scaledBytes));
    return false;
  }

  memset(raw, 0xFF, scaledBytes);

  const int srcStride = (srcW + 3) / 4;
  const float xRatio = static_cast<float>(srcW) / static_cast<float>(targetW);
  const float yRatio = static_cast<float>(srcH) / static_cast<float>(targetH);

  for (int ty = 0; ty < targetH; ++ty) {
    const int srcRenderY = static_cast<int>(ty * yRatio);
    const int srcRow = topDown ? srcRenderY : (srcH - 1 - srcRenderY);
    const int clampedRow = std::clamp(srcRow, 0, srcH - 1);
    const uint8_t* srcRowPtr = src2bpp + clampedRow * srcStride;
    uint8_t* dstRowPtr = raw + ty * scaledStride;

    for (int tx = 0; tx < targetW; ++tx) {
      const int srcX = static_cast<int>(tx * xRatio);
      const uint8_t val = (srcRowPtr[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;
      if (val < 3) {
        dstRowPtr[tx / 8] &= ~(1 << (7 - (tx % 8)));
      }
    }
  }

  entry.scaledPixels = BitmapCacheManager::ArenaBuf(raw, BitmapCacheManager::ArenaFree{&arena});
  entry.scaledPixelsBytes = scaledBytes;
  entry.scaledWidth = targetW;
  entry.scaledHeight = targetH;
  return true;
}

// Looks up `path` in `cache` (loading on miss), then blits it. `Opaque=false`
// (the default) writes only the black-source pixels, leaving white-source
// pixels showing whatever was underneath; `Opaque=true` writes both inks so
// the caller can skip a substrate fillRect. The `if constexpr` switch
// ensures each specialization carries exactly one inner-write branch.
template <bool Opaque>
bool GfxRenderer::drawCachedBitmap(BitmapCacheManager& cache, const char* path, const int x, const int y,
                                   const int maxWidth, const int maxHeight, const int cornerRadius) const {
  auto* entry = cache.get(path);
  if (entry == nullptr) return false;

  int targetW = 0;
  int targetH = 0;
  computeThumbTarget(entry->width, entry->height, maxWidth, maxHeight, targetW, targetH);
  if (targetW <= 0 || targetH <= 0) return false;

  // The prefetch worker pre-scales each cover to its draw dims using the same
  // computeThumbTarget(), so a populated entry matches and we blit directly. A
  // missing or differently-sized raster means the cover hasn't been
  // (re)rasterized for the current dims yet (e.g. right after a theme /
  // orientation change) — return false so the caller paints a placeholder until
  // the worker refills.
  if (!entry->scaledPixels || entry->scaledWidth != targetW || entry->scaledHeight != targetH) {
    return false;
  }

  constexpr BlitMode mode = Opaque ? BlitMode::Opaque : BlitMode::TransparentBlack;
  blit1Bit<mode>(entry->scaledPixels.get(), (targetW + 7) / 8, targetW, targetH, x, y, cornerRadius);
  return true;
}

template <GfxRenderer::BlitMode Mode>
void GfxRenderer::blit1Bit(const uint8_t* src, const int srcStride, const int w, const int h, const int x,
                           const int y, const int cornerRadius) const {
  // Corner-skip table. When r > 0, pixels in the four [0, r) × [0, r) corner
  // boxes that fall outside the rounded shape are left untouched — same
  // pixel test as maskRoundedRectOutsideCorners, applied here so the blit
  // never writes those pixels (no follow-up mask needed; any drop shadow
  // underneath remains visible). r is clamped to half the smaller drawn
  // dimension so opposite corners can't overlap.
  constexpr int kMaxCornerR = 32;
  int8_t skipPerRow[kMaxCornerR];
  const int rMax = std::min(kMaxCornerR, std::min(w / 2, h / 2));
  const int r = std::max(0, std::min(cornerRadius, rMax));
  const bool rounded = r > 0;
  if (rounded) {
    const int rr = r - 1;
    const int rr2 = rr * rr;
    for (int dy = 0; dy < r; ++dy) {
      const int ty = rr - dy;
      const int ty2 = ty * ty;
      int skip = 0;
      while (skip < r) {
        const int tx = rr - skip;
        if (tx * tx + ty2 > rr2)
          ++skip;
        else
          break;
      }
      skipPerRow[dy] = static_cast<int8_t>(skip);
    }
  }

  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int sx0 = std::max(0, -x);
  const int sx1 = std::min(w, screenW - x);
  const int sy0 = std::max(0, -y);
  const int sy1 = std::min(h, screenH - y);
  if (sx0 >= sx1 || sy0 >= sy1) return;

  int phyX0, phyY0;
  rotateCoordinates(orientation, x + sx0, y + sy0, &phyX0, &phyY0, panelWidth, panelHeight);

  // Per-orientation specializations:
  //   Portrait/PortraitInverted: phyX is constant within a row → hoist
  //     the dst byte column and bitMask out of the inner loop; the
  //     framebuffer write stride is ±panelWidthBytes per source-x step.
  //   LandscapeClockwise/CCW: phyY is constant within a row → hoist
  //     the dst row base; only the column varies, marching ±1 per step.
  // Source bits are walked byte-by-byte with a single shifting mask so
  // each pixel costs an AND + branch instead of recomputing (0x80 >> n).
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  switch (orientation) {
    case Portrait:
    case PortraitInverted: {
      const int dyPerSx = (orientation == Portrait) ? -1 : 1;
      const int dxPerSy = (orientation == Portrait) ? 1 : -1;
      const int32_t byteStep = static_cast<int32_t>(dyPerSx) * panelStride;

      for (int sy = sy0; sy < sy1; ++sy) {
        int rowSx0 = sx0;
        int rowSx1 = sx1;
        if (rounded) {
          int skip = 0;
          if (sy < r)
            skip = skipPerRow[sy];
          else if (sy >= h - r)
            skip = skipPerRow[h - 1 - sy];
          if (skip > 0) {
            rowSx0 = std::max(sx0, skip);
            rowSx1 = std::min(sx1, w - skip);
            if (rowSx0 >= rowSx1) continue;
          }
        }

        const int phyX = phyX0 + (sy - sy0) * dxPerSy;
        const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (phyX & 7));
        int32_t byteIndex =
            static_cast<int32_t>(phyY0) * panelStride + (phyX >> 3) + static_cast<int32_t>(rowSx0 - sx0) * byteStep;

        const uint8_t* srcRow = src + sy * srcStride;
        int sx = rowSx0;
        while (sx < rowSx1) {
          const int bitOffset = sx & 7;
          const int run = std::min(8 - bitOffset, rowSx1 - sx);
          uint8_t srcByte = srcRow[sx >> 3];
          uint8_t srcMask = static_cast<uint8_t>(0x80u >> bitOffset);
          for (int b = 0; b < run; ++b) {
            const bool srcSet = (srcByte & srcMask) != 0;
            if constexpr (Mode == BlitMode::Opaque) {
              if (srcSet)
                frameBuffer[byteIndex] |= bitMask;
              else
                frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else if constexpr (Mode == BlitMode::TransparentBlack) {
              if (!srcSet) frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else {  // TransparentWhite
              if (!srcSet) frameBuffer[byteIndex] |= bitMask;
            }
            byteIndex += byteStep;
            srcMask >>= 1;
          }
          sx += run;
        }
      }
      break;
    }

    case LandscapeClockwise:
    case LandscapeCounterClockwise: {
      const int dxPerSx = (orientation == LandscapeCounterClockwise) ? 1 : -1;
      const int dyPerSy = (orientation == LandscapeCounterClockwise) ? 1 : -1;

      for (int sy = sy0; sy < sy1; ++sy) {
        int rowSx0 = sx0;
        int rowSx1 = sx1;
        if (rounded) {
          int skip = 0;
          if (sy < r)
            skip = skipPerRow[sy];
          else if (sy >= h - r)
            skip = skipPerRow[h - 1 - sy];
          if (skip > 0) {
            rowSx0 = std::max(sx0, skip);
            rowSx1 = std::min(sx1, w - skip);
            if (rowSx0 >= rowSx1) continue;
          }
        }

        const int phyY = phyY0 + (sy - sy0) * dyPerSy;
        const int32_t rowBase = static_cast<int32_t>(phyY) * panelStride;
        int phyX = phyX0 + (rowSx0 - sx0) * dxPerSx;

        const uint8_t* srcRow = src + sy * srcStride;
        int sx = rowSx0;
        while (sx < rowSx1) {
          const int bitOffset = sx & 7;
          const int run = std::min(8 - bitOffset, rowSx1 - sx);
          uint8_t srcByte = srcRow[sx >> 3];
          uint8_t srcMask = static_cast<uint8_t>(0x80u >> bitOffset);
          for (int b = 0; b < run; ++b) {
            const bool srcSet = (srcByte & srcMask) != 0;
            const int32_t byteIndex = rowBase + (phyX >> 3);
            const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (phyX & 7));
            if constexpr (Mode == BlitMode::Opaque) {
              if (srcSet)
                frameBuffer[byteIndex] |= bitMask;
              else
                frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else if constexpr (Mode == BlitMode::TransparentBlack) {
              if (!srcSet) frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else {  // TransparentWhite
              if (!srcSet) frameBuffer[byteIndex] |= bitMask;
            }
            phyX += dxPerSx;
            srcMask >>= 1;
          }
          sx += run;
        }
      }
      break;
    }
  }
}

template bool GfxRenderer::drawCachedBitmap<false>(BitmapCacheManager&, const char*, int, int, int, int, int) const;
template bool GfxRenderer::drawCachedBitmap<true>(BitmapCacheManager&, const char*, int, int, int, int, int) const;

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
  display.clearScreen(color);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  if (displaySuppressed_) return;
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";

  std::string item = text;
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
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

  const auto fontIt = resolveFontIt(fontId);
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

  const auto fontIt = resolveFontIt(fontId);
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
  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);         // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = resolveFontIt(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = resolveFontIt(fontId);
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

  const auto fontIt = resolveFontIt(fontId);
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
    if (utf8IsCombiningMark(cp)) {
      const EpdFontData* combiningData = nullptr;
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style, &combiningData);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::centerOverRotated90CW(lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, combiningGlyph, combiningData, combiningX,
                                                combiningY, black);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdFontData* glyphData = nullptr;
    const EpdGlyph* glyph = font.getGlyph(cp, style, &glyphData);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, glyph, glyphData, x, lastBaseY, black);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const { display.displayGrayBuffer(fadingFix); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * Pairs with `storeBwBuffer` to restore the BW framebuffer after a grayscale render.
 * Uses chunked restoration to match chunked storage.
 *
 * If `storeBwBuffer` failed (chunks missing), the caller is contractually
 * required to have skipped the grayscale render — so the framebuffer still
 * holds the intended BW state. We still call `cleanupGrayscaleBuffers` to
 * rebase the display controller's BW/RED RAM from the framebuffer; skipping
 * it leaves the controller's prior-frame snapshot stale and the next
 * differential refresh ghosts the previous page.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    display.cleanupGrayscaleBuffers(frameBuffer);
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

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
