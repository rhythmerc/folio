#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <SdCardFont.h>
#include <SdFontGlyphCache.h>

FontCacheManager::FontCacheManager(const std::map<int, SdCardFont*>& sdCardFonts) : sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::setMemoryConstrained(bool constrained) {
  // Small enough to free most of the ~44 KB pool, non-zero so progress-screen
  // text (if it uses an SD theme font) still paints, just with more SD reads.
  static constexpr uint32_t CONSTRAINED_GLYPH_BYTES = 8 * 1024;
  static constexpr uint32_t CONSTRAINED_KERN_BYTES = 2 * 1024;

  auto& glyphCache = SdFontGlyphCache::getInstance();
  if (constrained) {
    glyphCache.setBudgets(CONSTRAINED_GLYPH_BYTES, CONSTRAINED_KERN_BYTES);
    // Built-in-font decompressor cache has no tunable budget; clear it once
    // (re-warms on the next render) to reclaim its heap for the operation.
    if (fontDecompressor_) fontDecompressor_->clearCache();
    // Advance tables aren't touched by clearCache() (they persist across layout
    // passes); drop them here to reclaim up to ~18 KB/font. They rebuild lazily.
    for (auto& [id, font] : sdCardFonts_) {
      font->clearPersistentCache();
    }
  } else {
    glyphCache.setBudgets(SdFontGlyphCache::DEFAULT_GLYPH_BUDGET_BYTES,
                          SdFontGlyphCache::DEFAULT_KERN_ROW_BUDGET_BYTES);
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
