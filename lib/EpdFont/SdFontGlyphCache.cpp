#include "SdFontGlyphCache.h"

SdFontGlyphCache& SdFontGlyphCache::getInstance() {
  static SdFontGlyphCache instance;
  return instance;
}

const EpdGlyph* SdFontGlyphCache::findGlyph(const SdCardFont* owner, uint8_t styleIdx, uint32_t codepoint) {
  for (auto& e : glyphs_) {
    if (e.owner == owner && e.styleIdx == styleIdx && e.codepoint == codepoint) {
      e.lastUsedTick = glyphTick_++;
      return &e.glyph;
    }
  }
  return nullptr;
}

const EpdGlyph* SdFontGlyphCache::insertGlyph(const SdCardFont* owner, uint8_t styleIdx, uint32_t codepoint,
                                              const EpdGlyph& glyph, std::unique_ptr<uint8_t[]> bitmap) {
  // evict-to-fit first so the push_back that returns the pointer is the final
  // mutation (pointer valid until the next insert, per the handler contract).
  evictGlyphsToFit(glyph.dataLength);
  glyphs_.push_back(GlyphEntry{});
  GlyphEntry& e = glyphs_.back();
  e.owner = owner;
  e.glyph = glyph;
  e.bitmap = bitmap.release();
  e.codepoint = codepoint;
  e.styleIdx = styleIdx;
  e.lastUsedTick = glyphTick_++;
  glyphBytes_ += glyph.dataLength;
  return &e.glyph;
}

bool SdFontGlyphCache::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (const auto& e : glyphs_) {
    if (&e.glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdFontGlyphCache::bitmapFor(const EpdGlyph* glyph) const {
  for (const auto& e : glyphs_) {
    if (&e.glyph == glyph) return e.bitmap;
  }
  return nullptr;
}

const int8_t* SdFontGlyphCache::findKernRow(const SdCardFont* owner, uint8_t styleIdx, uint8_t leftClass) {
  for (auto& e : kernRows_) {
    if (e.owner == owner && e.styleIdx == styleIdx && e.leftClass == leftClass) {
      e.lastUsed = kernTick_++;
      return e.row.data();
    }
  }
  return nullptr;
}

const int8_t* SdFontGlyphCache::insertKernRow(const SdCardFont* owner, uint8_t styleIdx, uint8_t leftClass,
                                              const int8_t* row, uint16_t rowWidth) {
  evictKernRowsToFit(rowWidth);
  kernRows_.push_back(KernRowEntry{});
  KernRowEntry& e = kernRows_.back();
  e.owner = owner;
  e.styleIdx = styleIdx;
  e.leftClass = leftClass;
  e.lastUsed = kernTick_++;
  e.row.assign(row, row + rowWidth);
  kernRowBytes_ += rowWidth;
  return e.row.data();
}

void SdFontGlyphCache::evictGlyphsToFit(uint32_t neededBytes) {
  while (!glyphs_.empty() &&
         (glyphs_.size() >= OVERFLOW_MAX_SLOTS || glyphBytes_ + neededBytes > glyphBudgetBytes_)) {
    size_t lru = 0;
    for (size_t i = 1; i < glyphs_.size(); i++) {
      if (glyphs_[i].lastUsedTick < glyphs_[lru].lastUsedTick) lru = i;
    }
    glyphBytes_ -= glyphs_[lru].glyph.dataLength;
    delete[] glyphs_[lru].bitmap;
    // O(1) removal: move the tail entry into the hole (shallow pointer copy; no
    // destructor runs, so bitmap ownership transfers cleanly), then drop it.
    glyphs_[lru] = glyphs_.back();
    glyphs_.pop_back();
  }
}

void SdFontGlyphCache::evictKernRowsToFit(uint32_t neededBytes) {
  while (!kernRows_.empty() &&
         (kernRows_.size() >= KERN_ROW_MAX_SLOTS || kernRowBytes_ + neededBytes > kernRowBudgetBytes_)) {
    size_t lru = 0;
    for (size_t i = 1; i < kernRows_.size(); i++) {
      if (kernRows_[i].lastUsed < kernRows_[lru].lastUsed) lru = i;
    }
    kernRowBytes_ -= static_cast<uint32_t>(kernRows_[lru].row.size());
    kernRows_[lru] = std::move(kernRows_.back());
    kernRows_.pop_back();
  }
}

void SdFontGlyphCache::setBudgets(uint32_t glyphBytes, uint32_t kernRowBytes) {
  glyphBudgetBytes_ = glyphBytes;
  kernRowBudgetBytes_ = kernRowBytes;
  // Trim now: evict-to-fit with no new item pending drops LRU entries until
  // resident bytes are under the (possibly reduced) budget.
  evictGlyphsToFit(0);
  evictKernRowsToFit(0);
}

void SdFontGlyphCache::clearOwner(const SdCardFont* owner) {
  for (size_t i = 0; i < glyphs_.size();) {
    if (glyphs_[i].owner == owner) {
      glyphBytes_ -= glyphs_[i].glyph.dataLength;
      delete[] glyphs_[i].bitmap;
      glyphs_[i] = glyphs_.back();
      glyphs_.pop_back();
    } else {
      ++i;
    }
  }
  for (size_t i = 0; i < kernRows_.size();) {
    if (kernRows_[i].owner == owner) {
      kernRowBytes_ -= static_cast<uint32_t>(kernRows_[i].row.size());
      kernRows_[i] = std::move(kernRows_.back());
      kernRows_.pop_back();
    } else {
      ++i;
    }
  }
}

void SdFontGlyphCache::clear() {
  for (auto& e : glyphs_) delete[] e.bitmap;
  glyphs_.clear();
  glyphs_.shrink_to_fit();
  glyphBytes_ = 0;
  glyphTick_ = 1;

  kernRows_.clear();
  kernRows_.shrink_to_fit();
  kernRowBytes_ = 0;
  kernTick_ = 1;
}
