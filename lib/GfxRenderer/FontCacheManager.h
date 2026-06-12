#pragma once

#include <map>

class FontDecompressor;
class SdCardFont;

// Owns the font glyph caches' shared lifecycle: clears them on demand and
// hands GfxRenderer the FontDecompressor for compressed-font bitmap lookups.
// Glyph caches are self-warming (SdCardFont overflow + kern-row caches,
// FontDecompressor group-LRU), so there is no prewarm/scan pass.
class FontCacheManager {
 public:
  explicit FontCacheManager(const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void logStats(const char* label = "render");
  void resetStats();

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

 private:
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
};
