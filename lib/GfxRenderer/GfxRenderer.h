#pragma once

#include <BitmapCacheManager.h>
#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "Bitmap1Bit.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

  enum Truncate {
    Start,
    End
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  std::map<int, EpdFontFamily>::const_iterator resolveFontIt(int fontId) const;

  // ID of the registered EpdFontFamily that serves as the system-wide glyph
  // fallback. When a font's subset misses a codepoint (typical for theme fonts
  // built with the UI-codepoint subset), EpdFont::getGlyph routes the lookup to
  // this family's regular EpdFont before substituting REPLACEMENT_GLYPH. 0 means
  // no fallback is wired (legacy behavior: missing codepoints render as tofu).
  int glyphFallbackFontId_ = 0;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;

  // While true, displayBuffer() is a no-op. Lets an overlay (e.g. GlobalMenu)
  // draw a base frame into the framebuffer without pushing it, then composite
  // and push exactly once. Mutable because displayBuffer() is const. Toggled
  // only via FlushGuard.
  mutable bool displaySuppressed_ = false;

  // See setBwImageCacheEnabled(): hint for image blocks to keep a BW RAM copy.
  bool bwImageCacheEnabled_ = false;


 public:
  // RAII: silence displayBuffer() for its lifetime so a base frame can be drawn
  // without a panel push, to be composited under an overlay before a single
  // push. Nested class — has access to the private displaySuppressed_ flag.
  class FlushGuard {
    const GfxRenderer& r_;

   public:
    explicit FlushGuard(const GfxRenderer& r) : r_(r) { r_.displaySuppressed_ = true; }
    ~FlushGuard() { r_.displaySuppressed_ = false; }
    FlushGuard(const FlushGuard&) = delete;
    FlushGuard& operator=(const FlushGuard&) = delete;
  };

  // Build the 1-bit raster for `entry` at (targetW × targetH) from a 2bpp
  // source buffer, allocating the raster from `arena`. Sets entry.scaledPixels
  // / scaledWidth / scaledHeight / scaledPixelsBytes; leaves entry.width /
  // height as the (caller-supplied) source dims. Returns false on arena OOM.
  // Called by the prefetch worker under the cache lock.
  bool buildScaledFromSource(BitmapCacheManager::Entry& entry, const uint8_t* src2bpp, int srcW, int srcH,
                             bool topDown, int targetW, int targetH, Arena& arena) const;

  // Decode the 1-bit BMP at `path` into the caller-supplied `dst` 2bpp buffer
  // (Bitmap::readNextRow format, stride = (w+3)/4). No allocation — `dstCap` is
  // the buffer size; decode fails if the image needs more. Stateless and
  // thread-safe (SD I/O goes through HalStorage's own mutex), so off-render-task
  // loaders can call it. Returns false on any failure (missing, non-1-bit,
  // too-large, decode error); on success fills outW/outH/outTopDown.
  static bool decodeThumbInto(const char* path, uint8_t* dst, std::size_t dstCap, int& outW, int& outH,
                              bool& outTopDown);

  // Target draw dimensions for a (srcW × srcH) bitmap aspect-fit into a
  // (maxW × maxH) envelope, scaling DOWN only. Shared by the prefetch worker
  // (to pre-scale) and drawCachedBitmap (to match), so both agree byte-for-byte.
  static void computeThumbTarget(int srcW, int srcH, int maxW, int maxH, int& outW, int& outH);

 private:
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();

  // Per-pixel compositing op for the shared 1-bit blit (blit1Bit). The source
  // convention is `0` = ink, `1` = background.
  //   TransparentBlack: paint ink black, leave background untouched.
  //   Opaque:           paint ink black and background white (both inks).
  //   TransparentWhite: paint ink white, leave background untouched.
  enum class BlitMode : uint8_t { TransparentBlack, Opaque, TransparentWhite };

  // Orientation-aware, pixel-exact 1-bit blit. `src` is an MSB-first packed
  // raster of `w`×`h` with row stride `srcStride` bytes, drawn at logical
  // (x, y). cornerRadius > 0 skips pixels outside the rounded corners. Shared
  // by drawCachedBitmap (pre-scaled covers) and the Bitmap1Bit drawIcon path.
  template <BlitMode Mode>
  void blit1Bit(const uint8_t* src, int srcStride, int w, int h, int x, int y, int cornerRadius) const;
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color, bool transparent = false>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }

  // Install the system-wide glyph fallback. `fontId` must already be registered
  // via insertFont. Retro-wires its regular-style EpdFont into every other
  // registered family so existing fonts inherit the fallback. Subsequent
  // insertFont calls also pick up the fallback automatically.
  void setGlyphFallbackFont(int fontId);
  int getGlyphFallbackFontId() const { return glyphFallbackFontId_; }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  // Fast 1px-tall half-tone rule from (x, y) spanning `length` logical pixels.
  // The dither is keyed to logical x only (ink on every other pixel), so a
  // single-row line is always ~50% inked regardless of y — unlike
  // fillRectDither(..., height=1, LightGray), whose pattern needs an even y and
  // vanishes on odd rows. Uses fillRectDither's rotate-once + direct-framebuffer
  // technique (no per-pixel drawPixel / rotateCoordinates overhead).
  void drawDitheredLine(int x, int y, int length) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;

  template <bool transparent = false>
  void fillRectDither(int x, int y, int width, int height, Color color) const;


  void drawCircle(int cx, int cy, int radius, Color color) const;
  void fillCircle(int cx, int cy, int radius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;

  // Legacy transparent icon blit: byte-aligned copy via the SDK. Fast, but the
  // start position is quantized to 8px on the packed axis and the source is not
  // rotated, so it is only correct in Portrait. Kept for existing list/menu
  // callers. Prefer the Bitmap1Bit overload below for new code.
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;

  // Precise transparent icon blit. Pixel-exact and orientation-aware (shares
  // the blit core with drawCachedBitmap), so it is not byte-quantized. A `0`
  // source bit is ink; `1` bits show whatever was painted underneath.
  //
  // inverted=false: ink is painted black (normal, light background).
  // inverted=true:  ink is painted white instead (for a dark/selected row).
  void drawIcon(const Bitmap1Bit& icon, int x, int y, bool inverted = false) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;

  // Opaque blit of a full logical-space 1-bit raster (MSB-first, `0` = black /
  // `1` = white, row stride `srcStride`) at logical (x, y). Orientation-aware
  // fast path (the shared blit1Bit core). Writes the full framebuffer, so use
  // only for full-frame BW — not during a grayscale strip pass.
  void blitImage1Bit(const uint8_t* src, int srcStride, int w, int h, int x, int y) const;

  // ─── Image cache ─────────────────────────────────────────────────────
  // Path-keyed bitmap cache. The renderer doesn't own the storage — the
  // caller (typically an activity that knows its working set) supplies a
  // BitmapCacheManager sized to the scene, populated by a prefetch worker that
  // pre-scales each cover to its draw dimensions. The render path only reads
  // (never decodes): a miss falls through to a placeholder while the cover is
  // in flight. When the working set rotates, the owner calls
  // BitmapCacheManager::clear() / evictIf() before re-populating.
  //
  // Read-only lookup: returns the bitmap's native dimensions on a cache hit,
  // false on a miss (no load triggered).
  bool peekCachedBitmapDimensions(BitmapCacheManager& cache, const char* path, int* outWidth, int* outHeight) const;

  // Draw the pre-scaled raster cached for `path` at top-left (x, y), aspect-fit
  // into (maxWidth × maxHeight). Returns false on a miss or if the cached raster
  // isn't sized for the requested envelope (cover not yet (re)rasterized) so the
  // caller can show a placeholder. 1-bit rasters only.
  //
  // Opaque=false (default): writes only the black-source pixels into the
  // framebuffer, leaving white-source pixels showing whatever was painted
  // underneath. Cheapest when the caller already laid down a known
  // background.
  //
  // Opaque=true: writes both inks.
  //
  // cornerRadius > 0: skip pixels in the four corner [0, r) × [0, r) boxes
  // that fall outside the rounded shape (same `tx²+ty² > rr²` test as
  // maskRoundedRectOutsideCorners). Leaves whatever was painted underneath
  // visible in those corners — cheaper and more correct for drop-shadowed
  // covers than drawing rectangular then masking with the background color
  // (a mask paints over the shadow; a skip lets it show through).
  template <bool Opaque = false>
  bool drawCachedBitmap(BitmapCacheManager& cache, const char* path, int x, int y, int maxWidth, int maxHeight,
                        int cornerRadius = 0) const;

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR, Truncate truncation = Truncate::End) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }

  // Transient hint: when set, image blocks may build/keep a RAM-resident BW
  // copy of their pixels (for fast re-renders, e.g. under the GlobalMenu)
  // instead of re-streaming from SD. Only honored in BW render mode.
  void setBwImageCacheEnabled(bool enabled) { bwImageCacheEnabled_ = enabled; }
  bool bwImageCacheEnabled() const { return bwImageCacheEnabled_; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
