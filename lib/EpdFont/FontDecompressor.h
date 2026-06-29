#pragma once

#include <InflateReader.h>

#include <vector>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns a pointer to the decompressed bitmap data for the given glyph,
  // decompressing its group on demand (cached in the group LRU).
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached groups + the persistent handle's scratch.
  void clearCache();

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint32_t hotGroupBytes = 0;    // total resident group-cache bytes
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Decompressed-group LRU. Each entry holds one byte-aligned decompressed
  // group; individual glyphs are compacted on demand into hotGlyphBuf. Keyed by
  // (font, groupIndex), bounded by GROUP_CACHE_BUDGET_BYTES and MAX_GROUP_SLOTS,
  // LRU-evicted via lruClock_. A UI screen only touches the group-0 of each
  // active (size, style); keeping them all resident eliminates the re-inflate
  // thrash that occurred when consecutive drawText calls alternated fonts
  // against a single decompressed-group slot.
  static constexpr uint32_t GROUP_CACHE_BUDGET_BYTES = 32 * 1024;
  static constexpr uint8_t MAX_GROUP_SLOTS = 16;
  struct GroupCacheEntry {
    const EpdFontData* font = nullptr;
    uint16_t groupIndex = UINT16_MAX;
    std::vector<uint8_t> data;
    uint32_t lastUsed = 0;
  };
  std::vector<GroupCacheEntry> groupCache_;
  uint32_t groupCacheBytes_ = 0;
  uint32_t lruClock_ = 0;

  // Scratch buffer for compacting a single glyph out of a resident group.
  // Valid until the next getBitmap() call.
  std::vector<uint8_t> hotGlyphBuf;

  void freeGroupCache();
  // Look up (font, groupIndex) in the LRU; decompress and insert on miss.
  // Returns the resident byte-aligned group, or nullptr on allocation failure.
  const std::vector<uint8_t>* getDecompressedGroup(const EpdFontData* fontData, uint16_t groupIndex);
  // Evict least-recently-used groups until a new group of neededBytes fits
  // within the byte budget and slot cap.
  void evictGroupsToFit(uint32_t neededBytes);
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height,
                                 bool is2Bit);
};
