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

  // Shrink (constrained=true) or restore (false) the SD-font RAM footprint for
  // the duration of a heap-hungry operation. Constrained mode drops the shared
  // glyph/kern-row cache to a small budget AND wipes each SD font's persistent
  // advance table (~18 KB/font, otherwise held until unload). Everything
  // re-warms lazily from SD afterwards. Prefer the ScopedFontMemoryBudget guard.
  void setMemoryConstrained(bool constrained);

  void logStats(const char* label = "render");
  void resetStats();

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

 private:
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
};

// RAII: constrain SD-font caches for the lifetime of a heap-hungry operation
// (network transfer, OTA), restore on scope exit. Null-safe — pass the result
// of GfxRenderer::getFontCacheManager() directly.
class ScopedFontMemoryBudget {
 public:
  explicit ScopedFontMemoryBudget(FontCacheManager* fcm) : fcm_(fcm) {
    if (fcm_) fcm_->setMemoryConstrained(true);
  }
  ~ScopedFontMemoryBudget() {
    if (fcm_) fcm_->setMemoryConstrained(false);
  }
  ScopedFontMemoryBudget(const ScopedFontMemoryBudget&) = delete;
  ScopedFontMemoryBudget& operator=(const ScopedFontMemoryBudget&) = delete;

 private:
  FontCacheManager* fcm_;
};
