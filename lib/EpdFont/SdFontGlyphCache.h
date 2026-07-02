#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "EpdFontData.h"  // EpdGlyph

// Owner key only — never dereferenced (compared for identity / tagging).
class SdCardFont;

// Global, demand-driven cache shared by ALL SdCardFont instances for their
// self-warming glyph bitmaps and kern-matrix rows.
//
// Why shared: the per-font caches it replaces budgeted ~44 KB EACH, so a theme
// that loaded N unique .cpfont files multiplied that (RoundedRaff's 4-5 fonts →
// ~180-220 KB) and OOM'd a 380 KB device. A single shared budget makes the
// footprint independent of font count; the hot font naturally occupies most of
// the pool while cold fonts use almost none.
//
// Lifetime: every entry is tagged with its owning SdCardFont. A font MUST call
// clearOwner(this) when it clears its caches or is destroyed, so no entry
// outlives its owner (the entries hold raw bitmap pointers and an owner key).
//
// Threading: touched only on the render task during drawText (the prefetch
// worker decodes cover bitmaps, not fonts) — no locking, same assumption the
// per-font caches relied on.
//
// Pointer contract: findGlyph/insertGlyph return a pointer into the entry
// vector. Per EpdFontData::glyphMissHandler, exactly one such pointer is
// outstanding at a time and is consumed (bitmap fetched + drawn) before the next
// miss, so a vector realloc/eviction on the next insert is safe.
class SdFontGlyphCache {
 public:
  static SdFontGlyphCache& getInstance();

  // Glyph overflow cache. find() bumps LRU on hit. insert() evicts to fit, then
  // stores a copy of `glyph` and takes ownership of `bitmap` (may be null for a
  // zero-width glyph). Both return the cached EpdGlyph*.
  const EpdGlyph* findGlyph(const SdCardFont* owner, uint8_t styleIdx, uint32_t codepoint);
  const EpdGlyph* insertGlyph(const SdCardFont* owner, uint8_t styleIdx, uint32_t codepoint, const EpdGlyph& glyph,
                              std::unique_ptr<uint8_t[]> bitmap);

  // Identity lookups for GfxRenderer::getGlyphBitmap. The EpdGlyph* originates
  // from find/insert above, so pointer identity is globally unique.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;
  const uint8_t* bitmapFor(const EpdGlyph* glyph) const;

  // Kern-matrix row cache (one row per left class, per font+style).
  const int8_t* findKernRow(const SdCardFont* owner, uint8_t styleIdx, uint8_t leftClass);
  const int8_t* insertKernRow(const SdCardFont* owner, uint8_t styleIdx, uint8_t leftClass, const int8_t* row,
                              uint16_t rowWidth);

  // Drop every entry owned by `owner` (call on clearCache / teardown), or all.
  void clearOwner(const SdCardFont* owner);
  void clear();

  // Default byte budgets (total across all fonts). Callers restore to these
  // after a constrained window; see FontCacheManager::setMemoryConstrained.
  static constexpr uint32_t DEFAULT_GLYPH_BUDGET_BYTES = 32 * 1024;
  static constexpr uint32_t DEFAULT_KERN_ROW_BUDGET_BYTES = 12 * 1024;

  // Runtime-adjust the byte budgets and immediately evict LRU entries down to
  // the new limits. Lets heap-hungry flows (network transfers, OTA) shrink the
  // pool for their duration, then restore it. Thrash in the shrunk window is
  // acceptable — glyphs re-warm from SD on the next paint.
  void setBudgets(uint32_t glyphBytes, uint32_t kernRowBytes);

 private:
  SdFontGlyphCache() = default;
  ~SdFontGlyphCache() { clear(); }
  SdFontGlyphCache(const SdFontGlyphCache&) = delete;
  SdFontGlyphCache& operator=(const SdFontGlyphCache&) = delete;

  // Single shared budgets (total across all fonts), matching the former
  // per-font values. The hot font can use most of the pool, so a shared 32 KB
  // outperforms 4 starved per-font slices at a quarter of the footprint.
  // Slot caps stay compile-time (secondary safety bound); the byte budgets are
  // the RAM lever and are runtime-tunable via setBudgets().
  static constexpr uint32_t OVERFLOW_MAX_SLOTS = 512;
  static constexpr uint32_t KERN_ROW_MAX_SLOTS = 96;
  uint32_t glyphBudgetBytes_ = DEFAULT_GLYPH_BUDGET_BYTES;
  uint32_t kernRowBudgetBytes_ = DEFAULT_KERN_ROW_BUDGET_BYTES;

  struct GlyphEntry {
    const SdCardFont* owner = nullptr;
    EpdGlyph glyph{};
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
    uint32_t lastUsedTick = 0;  // LRU timestamp; bumped on hit + insertion.
  };
  struct KernRowEntry {
    const SdCardFont* owner = nullptr;
    uint8_t styleIdx = 0;
    uint8_t leftClass = 0;
    uint32_t lastUsed = 0;
    std::vector<int8_t> row;  // kernRightClassCount values for this left class
  };

  std::vector<GlyphEntry> glyphs_;
  uint32_t glyphBytes_ = 0;
  uint32_t glyphTick_ = 1;  // monotonic (0 reserved as "unused" sentinel)

  std::vector<KernRowEntry> kernRows_;
  uint32_t kernRowBytes_ = 0;
  uint32_t kernTick_ = 1;

  // Evict LRU entries until a new item fits the slot cap + byte budget.
  void evictGlyphsToFit(uint32_t neededBytes);
  void evictKernRowsToFit(uint32_t neededBytes);
};
