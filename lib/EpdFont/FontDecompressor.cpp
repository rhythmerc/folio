#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdlib>

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() { freeGroupCache(); }

void FontDecompressor::clearCache() {
  freeGroupCache();
  // Reserve so push_back in getDecompressedGroup never reallocates — references
  // to a resident entry's data stay valid for the duration of a getBitmap call.
  groupCache_.reserve(MAX_GROUP_SLOTS);
}

void FontDecompressor::freeGroupCache() {
  groupCache_.clear();
  groupCache_.shrink_to_fit();
  groupCacheBytes_ = 0;
  lruClock_ = 0;
  hotGlyphBuf.clear();
  hotGlyphBuf.shrink_to_fit();
}

void FontDecompressor::evictGroupsToFit(uint32_t neededBytes) {
  while (!groupCache_.empty() && (groupCache_.size() >= MAX_GROUP_SLOTS ||
                                  groupCacheBytes_ + neededBytes > GROUP_CACHE_BUDGET_BYTES)) {
    size_t lruIdx = 0;
    for (size_t i = 1; i < groupCache_.size(); i++) {
      if (groupCache_[i].lastUsed < groupCache_[lruIdx].lastUsed) lruIdx = i;
    }
    groupCacheBytes_ -= groupCache_[lruIdx].data.size();
    groupCache_.erase(groupCache_.begin() + lruIdx);
  }
}

const std::vector<uint8_t>* FontDecompressor::getDecompressedGroup(const EpdFontData* fontData, uint16_t groupIndex) {
  for (auto& e : groupCache_) {
    if (e.font == fontData && e.groupIndex == groupIndex) {
      e.lastUsed = ++lruClock_;
      stats.cacheHits++;
      return &e.data;
    }
  }

  stats.cacheMisses++;
  const EpdFontGroup& group = fontData->groups[groupIndex];

  // Make room (also caps slot count). A single group larger than the whole
  // budget is still admitted — it just becomes the sole resident entry.
  evictGroupsToFit(group.uncompressedSize);

  // groupCache_ is reserved to MAX_GROUP_SLOTS, so emplace_back never
  // reallocates and the returned data pointer stays valid through this call.
  groupCache_.emplace_back();
  GroupCacheEntry& e = groupCache_.back();
  e.data.resize(group.uncompressedSize);
  if (e.data.size() != group.uncompressedSize) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for group %u", group.uncompressedSize, groupIndex);
    groupCache_.pop_back();
    return nullptr;
  }
  if (!decompressGroup(fontData, groupIndex, e.data.data(), group.uncompressedSize)) {
    groupCache_.pop_back();
    return nullptr;
  }
  e.font = fontData;
  e.groupIndex = groupIndex;
  e.lastUsed = ++lruClock_;
  groupCacheBytes_ += group.uncompressedSize;
  stats.hotGroupBytes = groupCacheBytes_;
  return &e.data;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;
  const bool is2Bit = fontData->is2Bit;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      // Byte-aligned row stride: 4 px/byte for 2-bit, 8 px/byte for 1-bit.
      const uint32_t rowStride = is2Bit ? ((g.width + 3) / 4) : ((g.width + 7) / 8);
      offset += rowStride * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height, bool is2Bit) {
  if (width == 0 || height == 0) return;
  // Reverse to_byte_aligned(): pack byte-aligned rows back into the continuous
  // bitstream renderCharImpl expects. bits/pixel and px/byte depend on depth.
  const uint8_t bits = is2Bit ? 2 : 1;
  const uint8_t pxPerByte = is2Bit ? 4 : 8;
  const uint8_t mask = is2Bit ? 0x3 : 0x1;
  const uint32_t rowStride = (width + pxPerByte - 1) / pxPerByte;
  if (width % pxPerByte == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      const uint8_t pixel = (alignedSrc[y * rowStride + x / pxPerByte] >> (((pxPerByte - 1) - (x % pxPerByte)) * bits)) & mask;
      outByte = (outByte << bits) | pixel;
      outBits += bits;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: decompressed-group LRU (on-demand) ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  const std::vector<uint8_t>* group = getDecompressedGroup(fontData, groupIndex);
  if (!group) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (glyph->dataLength > hotGlyphBuf.size()) {
    hotGlyphBuf.resize(glyph->dataLength);
  }
  if (hotGlyphBuf.empty()) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&(*group)[alignedOff], hotGlyphBuf.data(), glyph->width, glyph->height, fontData->is2Bit);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf.data();
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groupCache=%lu bytes", label, stats.decompressTimeMs, stats.hotGroupBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
