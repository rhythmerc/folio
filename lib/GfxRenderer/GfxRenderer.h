#pragma once

#include <BitmapCacheManager.h>
#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;
class TextCollector;

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

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

  // Active dry-run text collector. Non-null only while the render pipeline
  // is executing the prewarm pass (see ActivityManager::renderTaskLoop and
  // Activity::wantsPrewarmRender). When set, every drawText routes to
  // collector->use() instead of rasterizing, and every drawing primitive
  // (lines, rects, bitmaps, …) early-returns. Measurement APIs (getTextWidth,
  // wrappedText, kerning, etc.) still run normally so layout decisions are
  // deterministic across the dry-run and real-render passes.
  //
  // Mutable for the same reason as fontCacheManager_: the drawing methods
  // are const but the collector pointer is render-pipeline state, not
  // user-observable state.
  mutable TextCollector* prewarmTextCollector_ = nullptr;

  // Lazily populate `cache` with the decoded BMP at `path`. Returns the
  // cached entry, or nullptr on miss (file missing, unsupported format,
  // decode error, OOM). Callers own the cache; the renderer just decodes
  // into it.
  BitmapCacheManager::Entry* lookupOrLoadCachedBitmap(BitmapCacheManager& cache, const char* path) const;
  void buildScaledBitmap(BitmapCacheManager::Entry* entry, int targetW, int targetH) const;

 public:
  // Read and decode the BMP at `path` into a detached Entry that the caller can
  // hand to a BitmapCacheManager via set(). Stateless and thread-safe (SD I/O
  // goes through HalStorage's own mutex), so off-render-task loaders can use
  // it without touching any GfxRenderer instance state. Returns an Entry with
  // an empty `path` on failure (file missing, non-1-bit, decode error, OOM).
  static BitmapCacheManager::Entry decodeBitmapEntry(const char* path);

 private:
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
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

  // Install / clear the dry-run prewarm collector. Set to a non-null pointer
  // before invoking render() in dry-run mode, clear back to nullptr before
  // the real render. While set, every drawing primitive in this class
  // either records its text into the collector (text APIs) or returns
  // immediately (everything else).
  void setPrewarmTextCollector(TextCollector* c) const { prewarmTextCollector_ = c; }
  TextCollector* getPrewarmTextCollector() const { return prewarmTextCollector_; }
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
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void drawCircle(int cx, int cy, int radius, Color color) const;
  void fillCircle(int cx, int cy, int radius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;

  // ─── Image cache ─────────────────────────────────────────────────────
  // Path-keyed bitmap cache. The renderer doesn't own the storage — the
  // caller (typically an activity that knows its working set) supplies a
  // BitmapCacheManager sized to the scene. The first call for a (cache,
  // path) pair reads + decodes from SD into the cache; subsequent calls
  // render straight from the cached pixels. When the working set
  // rotates, the owner calls BitmapCacheManager::clear() before
  // re-populating.
  //
  // Returns the bitmap's native dimensions on success.
  bool getCachedBitmapDimensions(BitmapCacheManager& cache, const char* path, int* outWidth, int* outHeight) const;

  // Read-only sibling of getCachedBitmapDimensions: returns false on cache
  // miss instead of triggering a load. Used by lazy-loading callers that
  // populate the cache from a worker task and want the render path to fall
  // through to a placeholder while the cover is in flight.
  bool peekCachedBitmapDimensions(BitmapCacheManager& cache, const char* path, int* outWidth, int* outHeight) const;

  // Draw the BMP at `path`, scaled to fit (maxWidth × maxHeight) at top-
  // left (x, y). Reads + decodes + stores in `cache` on first call for a
  // given path; subsequent calls memcpy-style rasterize from cached
  // pixels. Returns true on success. Currently 1-bit BMPs only — higher-
  // bpp images fall through to the SD-backed drawBitmap path.
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
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
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
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
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
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
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
