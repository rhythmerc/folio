#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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

  // Cached bitmap entry. Treat as an opaque handle from outside the
  // renderer — callers obtain a pointer via lookupCachedBitmap() and
  // reuse it across dim queries / draw calls in the same paint to spare
  // repeated map lookups. Fields are exposed only so the struct can live
  // in the public scope alongside its handle accessors.
  //
  // Cached pixel data is in the 2-bit-per-pixel packed format that
  // Bitmap::readNextRow produces (consistent across 1/4/8 bpp sources).
  // Stride is (width + 3) / 4 bytes; pixels buffer size = stride × height.
  // Pixel ordering follows the bitmap's natural orientation — use
  // `topDown` to map render Y → buffer Y.
  struct CachedBitmap {
    std::unique_ptr<uint8_t[]> pixels;
    size_t pixelsBytes = 0;
    int width = 0;
    int height = 0;
    bool topDown = false;
    uint32_t lastUsedTick = 0;  // monotonic; touched on every cache lookup.

    // Pre-scaled 1-bit pixel data at the most recently requested target
    // dimensions. Built lazily on first drawCachedBitmap call; reused on
    // subsequent paints when target size matches. 1 bit/pixel, MSB first,
    // row-major, stride = (scaledWidth + 7) / 8.
    std::unique_ptr<uint8_t[]> scaledPixels;
    size_t scaledPixelsBytes = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
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
  // Mutable because the font miss handler (used by SdThemeLoader for lazy
  // theme-font restoration after the reader evicts) populates the map from
  // const lookup paths like getTextWidth() / getLineHeight(). Same pragmatic
  // compromise as sdCardFonts_ below.
  mutable std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Lazy-load hook: when a fontMap lookup misses, the renderer invokes this
  // handler with the missing fontId. If the handler returns true (meaning it
  // restored the font registration), the lookup is retried once. Used by
  // SdThemeLoader to lazily reload theme fonts evicted by the reader.
  using FontMissHandler = bool (*)(int fontId, void* ctx);
  FontMissHandler fontMissHandler_ = nullptr;
  void* fontMissCtx_ = nullptr;
  std::map<int, EpdFontFamily>::const_iterator resolveFontIt(int fontId) const;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // ─── Image cache state ──────────────────────────────────────────────
  // Transparent hash + key_equal so unordered_map::find(const char*) /
  // find(string_view) doesn't construct a temporary std::string for the
  // key — material on the Library paint path where 9 lookups/paint used to
  // each allocate. std::hash<string_view> and std::hash<string> are
  // guaranteed to agree on identical contents since C++17 LWG2912.
  struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    size_t operator()(const std::string& s) const noexcept { return std::hash<std::string>{}(s); }
  };
  struct TransparentStringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  mutable std::unordered_map<std::string, CachedBitmap, TransparentStringHash, TransparentStringEq>
      imageCache_;
  mutable size_t imageCacheBytes_ = 0;
  mutable size_t imageCacheBudget_ = 64 * 1024;
  mutable uint32_t imageCacheTick_ = 0;

  void buildScaledBitmap(CachedBitmap* entry, int targetW, int targetH) const;

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
  // Install a callback invoked when a fontMap lookup misses. The callback
  // may restore the registration and return true, in which case the lookup
  // is retried once. ctx is passed through unchanged.
  void setFontMissHandler(FontMissHandler handler, void* ctx) {
    fontMissHandler_ = handler;
    fontMissCtx_ = ctx;
  }
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
  // Path-keyed cache of decoded 1-bit BMP data. First call for a path
  // reads + decodes from SD into RAM; subsequent calls render straight
  // from the cached pixels. Avoids the per-paint SD-I/O cost (~30 ms per
  // cover on a 9-tile Library page) without requiring callers to manage
  // their own capture/restore buffers.
  //
  // The cache is LRU-managed under a configurable byte budget (~64 KB
  // default — enough for ~50 small thumbs). Activities call
  // clearImageCache() on exit when they want the RAM back.
  //
  // Returns the bitmap's native dimensions on success; pass nullptrs to
  // skip the dim outputs when you only care about cache priming.
  bool getCachedBitmapDimensions(const char* path, int* outWidth, int* outHeight) const;
  bool getCachedBitmapDimensions(const CachedBitmap* handle, int* outWidth, int* outHeight) const;

  // Draw the BMP at `path`, scaled to fit (maxWidth × maxHeight) at top-
  // left (x, y). Reads + decodes + caches on first call for a given path;
  // subsequent calls memcpy-style rasterize from cached pixels. Returns
  // true on success. Currently 1-bit BMPs only — higher-bpp images fall
  // through to the SD-backed drawBitmap path.
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
  bool drawCachedBitmap(const char* path, int x, int y, int maxWidth, int maxHeight,
                        int cornerRadius = 0) const;
  template <bool Opaque = false>
  bool drawCachedBitmap(CachedBitmap* handle, int x, int y, int maxWidth, int maxHeight,
                        int cornerRadius = 0) const;

  // Look the cache up by path, decoding on miss. Returns an opaque handle
  // suitable for reuse across getCachedBitmapDimensions + drawCachedBitmap
  // in the same paint, sparing a second map lookup (and the temporary
  // std::string the heterogeneous comparator would still allocate when
  // pulled in from the LRU-evict path). nullptr on failure (file not
  // found, unsupported format, OOM).
  CachedBitmap* lookupCachedBitmap(const char* path) const;

  // Drop every cached bitmap and reclaim the RAM.
  void clearImageCache() const;

  // Override the default cache budget (bytes). Eviction is LRU.
  void setImageCacheBudget(size_t bytes) const { imageCacheBudget_ = bytes; }
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
