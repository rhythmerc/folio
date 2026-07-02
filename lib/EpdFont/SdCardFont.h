#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

// Thread-safe SD file wrapper (aliased as FsFile downstream via HalStorage.h).
// Forward-declared here so the header doesn't pull in the HAL; the persistent
// handle member is a unique_ptr, completed in SdCardFont.cpp.
class HalFile;

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

// Low-level .cpfont (v4) loader and glyph cache. Shared by both the reader
// (one instance, via ReaderFontManager) and the UI/theme system (up to 8
// instances deduped by path, via ThemeFontManager). Each instance owns one
// open file path plus per-style glyph state.
//
// Rendering is self-warming: epdFont.data always points at stubData, and
// glyph bitmaps + kern-matrix rows are resolved on demand and cached:
//
//   - Glyphs: EpdFontData::glyphMissHandler → onGlyphMiss → byte-budgeted
//     overflow LRU (read through the persistent overflowFile_ handle).
//   - Kerning: EpdFontData::kernRowHandler → onKernRow → kern-row LRU.
//   - Ligatures + kern class tables: loaded once (loadStyleKernLigatureData)
//     and wired into stubData by applyGlyphMissCallback.
//
// clearCache() drops these caches (the handle too); freeAll() tears the
// instance down. There is no prewarm pass.
//
// A separate Reader-only layout API (buildAdvanceTable + getAdvance) maintains
// a persistent advance cache sized for a whole book layout pass — the UI never
// touches it (UI text has fixed positions). See ParsedText.cpp +
// TxtReaderActivity.cpp; GfxRenderer::ensureSdCardFontReady forwards into it.
class SdCardFont {
 public:
  static constexpr uint8_t MAX_STYLES = 4;

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // --- Reader-only layout API ----------------------------------------------
  // The advance table is exercised exclusively by the reader's layout pass
  // (ParsedText / TxtReaderActivity → GfxRenderer::ensureSdCardFontReady).
  // The UI never builds or queries it because UI text has fixed widths and
  // never re-flows. It's a separate, longer-lived cache keyed by codepoint.

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words, batch-reads advanceX from SD,
  // stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;
  // --- End reader-only layout API ------------------------------------------

  // Free mini data for all styles and restore stub EpdFontData.
  // Preserves the persistent advance cache so repeated layout passes can reuse
  // previously fetched metrics.
  void clearCache();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig tables, stub, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Resident interval table for codepoint lookup. Exactly one of these is
    // non-null once intervals are loaded. BMP-only fonts with <=65535 glyphs
    // (the common case: Latin/Cyrillic/Hebrew/Greek/Vietnamese) use the compact
    // 6-byte form, halving resident lookup metadata; large sparse CJK subsets
    // fall back to the full 12-byte table. The on-disk format is always 12-byte.
    EpdUnicodeInterval* fullIntervals = nullptr;
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    };
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;

    // Persistent kern-class + ligature tables (loaded once on first use).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; matrix rows are fetched on demand via onKernRow.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // The single EpdFontData this style renders from. Glyph bitmaps + kern rows
    // resolve on demand via the miss/row handlers wired in applyGlyphMissCallback.
    EpdFontData stubData{};

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // The self-warming glyph-overflow and kern-row caches live in the global
  // SdFontGlyphCache (a single shared budget across all fonts, so footprint is
  // independent of how many fonts a theme loads). onGlyphMiss / onKernRow read
  // a miss through overflowFile_ and insert it there, tagged by `this`;
  // clearOverflow() / the dtor purge this font's entries via clearOwner(this).

  // Persistent read handle for on-demand glyph + kern-row reads. Opened lazily
  // on first miss and reused across paints (closed by clearOverflow/freeAll).
  // Deep-sleep wake is a full chip reset, so no stale handle survives sleep.
  std::unique_ptr<HalFile> overflowFile_;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);
  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  // Lazy-load a style's kern-class + ligature tables AND wire them into its stub
  // (kern classes, on-demand row handler, ligatures). Called on the style's first
  // use via ensureStyleIntervalsLoaded — NOT at load() — so an unrendered size
  // never pays the ~18KB kern cost. Guarded by kernLigLoaded; no-op if loaded.
  bool loadStyleKernLigatureData(uint8_t styleIdx);
  // Lazy-load fullIntervals for a style. Returns true if intervals are
  // resident after the call. Cheap when already loaded (no-op).
  bool ensureStyleIntervalsLoaded(uint8_t styleIdx);
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  template <typename Iter>
  int buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Persistent read handle for the on-demand caches.
  HalFile* ensureOverflowFileOpen();
  void closeOverflowFile();

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
  // Static callback for EpdFontData::kernRowHandler — returns the kern matrix
  // row for a 1-based left class, reading + caching it on demand.
  static const int8_t* onKernRow(void* ctx, uint8_t leftClass);
};
