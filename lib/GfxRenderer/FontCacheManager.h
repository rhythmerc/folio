#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontCacheManager;
class FontDecompressor;
class SdCardFont;

// Declarative glyph preload — activities call `use()` from declareText() to
// describe what text they're about to render, and the collector batched-loads
// the needed glyphs into the font cache once.
//
// Aggregates declarations per fontId so multiple `use(fontId, …)` calls in
// one declareText pass collapse into a single prewarmCache(fontId, …) call
// at apply time. The styleMask is OR-ed across all uses for that font, and
// the text strings concatenated — the font cache's SdCardFont layer
// hashes the resulting (codepoints, mask) and short-circuits if it matches
// the last paint's declaration, so a stable scene costs only the hash.
class TextCollector {
 public:
  void use(int fontId, EpdFontFamily::Style style, const char* text);
  void use(int fontId, EpdFontFamily::Style style, const std::string& text) { use(fontId, style, text.c_str()); }

  // Flush accumulated declarations to the cache — called once by the
  // render pipeline after the activity's declareText returns.
  void applyTo(FontCacheManager& fcm) const;

 private:
  struct PerFont {
    std::string text;
    uint8_t styleMask = 0;
  };
  std::map<int, PerFont> byFont_;
};

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // Returns the underlying SdCardFont for fontId, or nullptr if fontId is
  // not an SD card font. Used by TextCollector to dedup prewarm calls when
  // multiple theme roles share one SdCardFont instance — the mini cache is
  // destructive on rebuild, so two prewarms with different text would have
  // the second clobber the first.
  SdCardFont* findSdCardFont(int fontId) const {
    auto it = sdCardFonts_.find(fontId);
    return (it != sdCardFonts_.end()) ? it->second : nullptr;
  }

  // RAII scope for two-pass prewarm pattern.
  //
  // Two modes:
  //  * Destructive (default, reader semantics): ctor and dtor each clearCache.
  //    Use when the prewarmed content is one-shot (page text in the reader) —
  //    every page wipes and rebuilds. Old behavior, unchanged.
  //  * Persistent (UI-render semantics): no destructive clears. Combined with
  //    SdCardFont's idempotent prewarm short-circuit, this makes per-render
  //    prewarm cheap when content is stable. The mini cache survives between
  //    paints; the next paint re-runs the scan, hashes the same codepoints,
  //    and skips the rebuild entirely.
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager, bool persistent = false);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
    bool persistent_ = false;
  };
  PrewarmScope createPrewarmScope();
  PrewarmScope createPersistentPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  std::string scanText_;
  uint32_t scanStyleCounts_[4] = {};
  int scanFontId_ = -1;
};
