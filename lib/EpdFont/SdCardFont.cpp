#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"
#include "SdFontGlyphCache.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// Walks a null-terminated UTF-8 string and appends each unique codepoint to
// codepoints[0..cpCount-1] via O(n²) dedup.  Returns true if the buffer
// reached maxCount (cap hit), false if all codepoints fit.
bool collectUniqueCodepoints(const char* text, uint32_t* codepoints, uint32_t& cpCount, uint32_t maxCount) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (cpCount >= maxCount) return true;
      codepoints[cpCount++] = cp;
    }
  }
  return false;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

}  // namespace

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  delete[] s.bmpIntervals;
  s.bmpIntervals = nullptr;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
}

void SdCardFont::clearOverflow() {
  // Drop this font's glyph + kern-row entries from the shared pool, then release
  // the persistent read handle (tied to the on-demand cache lifetime).
  SdFontGlyphCache::getInstance().clearOwner(this);
  closeOverflowFile();
}

HalFile* SdCardFont::ensureOverflowFileOpen() {
  if (overflowFile_ && overflowFile_->isOpen()) return overflowFile_.get();
  if (!overflowFile_) {
    overflowFile_ = std::unique_ptr<HalFile>(new (std::nothrow) FsFile());
    if (!overflowFile_) {
      LOG_ERR("SDCF", "OOM: overflow file handle");
      return nullptr;
    }
  }
  if (!Storage.openFileForRead("SDCF", filePath_, *overflowFile_)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for on-demand reads: %s", filePath_);
    return nullptr;
  }
  return overflowFile_.get();
}

void SdCardFont::closeOverflowFile() { overflowFile_.reset(); }

// --- Per-style kern/ligature ---

bool SdCardFont::loadStyleKernLigatureData(uint8_t styleIdx) {
  auto& s = styles_[styleIdx];
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is never resident — getKerning fetches
    // one matrix row per left class on demand via onKernRow.
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Wire the stub so getKerning resolves classes from the resident tables and
  // fetches matrix rows on demand via onKernRow (the full matrix is never
  // resident), and make ligatures visible. Wired HERE (not applyGlyphMissCallback)
  // so it happens on the style's lazy first-use load rather than at load() time.
  // overflowCtx_[styleIdx] was populated by applyGlyphMissCallback at load().
  if (s.kernLeftClasses && s.kernRightClasses) {
    s.stubData.kernLeftClasses = s.kernLeftClasses;
    s.stubData.kernRightClasses = s.kernRightClasses;
    s.stubData.kernLeftEntryCount = s.header.kernLeftEntryCount;
    s.stubData.kernRightEntryCount = s.header.kernRightEntryCount;
    s.stubData.kernLeftClassCount = s.header.kernLeftClassCount;
    s.stubData.kernRightClassCount = s.header.kernRightClassCount;
    s.stubData.kernMatrix = nullptr;  // resolved per-row on demand via onKernRow
    s.stubData.kernRowHandler = &SdCardFont::onKernRow;
    s.stubData.kernRowCtx = &overflowCtx_[styleIdx];
  }
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Lazy per-style interval loader ---

// fullIntervals are allocated and read on first access. Validation runs once
// here; subsequent calls are a cheap null check. Failures clear the partial
// allocation so a retry path stays consistent.
bool SdCardFont::ensureStyleIntervalsLoaded(uint8_t styleIdx) {
  if (styleIdx >= MAX_STYLES) return false;
  auto& s = styles_[styleIdx];
  if (!s.present) return false;
  if (s.fullIntervals || s.bmpIntervals) return true;

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for intervals: %s", filePath_);
    return false;
  }

  s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
  if (!s.fullIntervals) {
    LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, styleIdx);
    return false;
  }

  if (!file.seekSet(s.intervalsFileOffset)) {
    LOG_ERR("SDCF", "Failed to seek to intervals for style %u", styleIdx);
    delete[] s.fullIntervals;
    s.fullIntervals = nullptr;
    return false;
  }
  const size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
  if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
    LOG_ERR("SDCF", "Failed to read intervals for style %u", styleIdx);
    delete[] s.fullIntervals;
    s.fullIntervals = nullptr;
    return false;
  }

  // Validate interval contents before any later code (findGlobalGlyphIndex,
  // glyph reads) trusts them. A malformed file could otherwise drive
  // out-of-range glyph indices into bogus on-disk reads.
  // A BMP-only font (<=65535 glyphs, all interval bounds + offsets <=65535) can
  // drop to the 6-byte compact table after this validation pass.
  bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
  uint32_t expectedOffset = 0;
  uint32_t prevLast = 0;
  for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
    const auto& iv = s.fullIntervals[j];
    if (iv.first > iv.last) {
      LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", styleIdx, j,
              static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
      delete[] s.fullIntervals;
      s.fullIntervals = nullptr;
      return false;
    }
    const uint32_t span = iv.last - iv.first + 1;
    const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
    const bool spanTooBig = (span > s.header.glyphCount);
    const bool offsetMismatch = (iv.offset != expectedOffset);
    const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
    if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
      LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", styleIdx, j,
              overlapsPrev, span, offsetMismatch, offsetOverruns);
      delete[] s.fullIntervals;
      s.fullIntervals = nullptr;
      return false;
    }
    if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
      canUseBmp16 = false;
    }
    expectedOffset += span;
    prevLast = iv.last;
  }

  // Compact to the 6-byte table when the font fits in 16 bits, then drop the
  // 12-byte buffer. On OOM, keep the validated full table — correctness over
  // the saving.
  if (canUseBmp16) {
    s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[s.header.intervalCount];
    if (s.bmpIntervals) {
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        const auto& iv = s.fullIntervals[j];
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      delete[] s.fullIntervals;
      s.fullIntervals = nullptr;
    }
  }

  // First real touch of this style: warm its kern-class + ligature tables too
  // (best effort — getKerning/ligature lookups are null-safe if it fails). This
  // single hook is shared by paint (onGlyphMiss) and reader layout
  // (buildAdvanceTable), so kern/lig load only for sizes that actually render.
  loadStyleKernLigatureData(styleIdx);

  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];

  // Kern-class + ligature tables (and their stub wiring) are loaded lazily on
  // the style's first use — see ensureStyleIntervalsLoaded ->
  // loadStyleKernLigatureData. Loading them here (at load() time) would cost
  // ~18KB per size up front, which is prohibitive when eager-loading a whole
  // family. overflowCtx_ is populated above so the lazy wiring can reference it.
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Initialize per-style stub data and glyph-miss callback. fullIntervals are
  // NOT allocated here — they're lazy per style (see ensureStyleIntervalsLoaded)
  // so a multi-style .cpfont only pays for the styles actually rendered. On a
  // theme like RoundedRaff (4 styles × 4 deduped fonts × ~150 intervals × 12
  // bytes = ~28KB), an activity that only uses Regular saves ~21KB at boot.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const uint32_t first = s.bmpIntervals ? s.bmpIntervals[mid].first : s.fullIntervals[mid].first;
    const uint32_t last = s.bmpIntervals ? s.bmpIntervals[mid].last : s.fullIntervals[mid].last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = s.bmpIntervals ? s.bmpIntervals[mid].offset : s.fullIntervals[mid].offset;
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  // Drops the self-warming glyph + kern-row caches and the persistent handle.
  // The advance table is intentionally preserved (it persists across layout
  // passes so repeated section indexing amortizes SD reads); use
  // clearPersistentCache() to wipe it. epdFont.data stays pointed at stubData.
  clearOverflow();
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

void SdCardFont::mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount) {
  if (newCount == 0) return;
  const uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) return;  // already full

  // Cap the merged size at ADVANCE_CACHE_LIMIT. Anything past the cap is
  // dropped from the tail of the sorted merge — a deterministic, bounded loss
  // that doesn't bias which codepoints get cached on subsequent passes.
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  AdvanceEntry* merged = new (std::nothrow) AdvanceEntry[mergedCap];
  if (!merged) {
    LOG_ERR("SDCF", "mergeIntoAdvanceTable: alloc failed (%u entries) style %u", mergedCap, styleIdx);
    return;
  }

  const AdvanceEntry* a = advanceTable_[styleIdx];
  const AdvanceEntry* b = sortedNew;
  uint32_t i = 0, j = 0, k = 0;
  while (k < mergedCap && (i < oldSize || j < newCount)) {
    if (i < oldSize && (j >= newCount || a[i].codepoint <= b[j].codepoint)) {
      merged[k++] = a[i++];
    } else {
      merged[k++] = b[j++];
    }
  }

  delete[] advanceTable_[styleIdx];
  advanceTable_[styleIdx] = merged;
  advanceTableSize_[styleIdx] = k;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  if (!advanceTable_[style]) return 0;
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  return 0;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    if (!ensureStyleIntervalsLoaded(si)) {
      totalMissed += static_cast<int>(cpCount);
      continue;
    }
    const auto& s = styles_[si];

    // Stop fetching once the cache is full — further inserts would be dropped
    // by the merge anyway. The renderer fast path tolerates missing entries
    // (returns 0); the slow path is still correct for those codepoints.
    if (advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT) continue;

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (advanceTableLookup(si, cp, nullptr)) continue;  // already cached
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Open file once and read advanceX for each needed glyph.
    FsFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }

    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      file.close();
      continue;
    }

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
          break;
        }
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = tempGlyph.advanceX;
      fetched++;
    }
    file.close();

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }

    LOG_DBG("SDCF", "Advance table style %u: +%u from SD, total=%u/%u", si, fetched, advanceTableSize_[si],
            ADVANCE_CACHE_LIMIT);
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // +2 reserved slots for space and hyphen injected after the main scan.
  static constexpr uint32_t MAX_UNIQUE_CODEPOINTS = 4096;
  uint32_t* codepoints = new (std::nothrow) uint32_t[MAX_UNIQUE_CODEPOINTS + 2];
  if (!codepoints) {
    LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes)", MAX_UNIQUE_CODEPOINTS * 4);
    return -1;
  }
  uint32_t cpCount = 0;
  bool hitCap = false;

  for (auto it = begin; it != end && !hitCap; ++it) {
    hitCap = collectUniqueCodepoints(asCStr(*it), codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }

  if (includeSpace && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == ' '; }))
    codepoints[cpCount++] = ' ';
  if (includeHyphen && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == '-'; }))
    codepoints[cpCount++] = '-';

  if (hitCap) {
    LOG_ERR("SDCF", "buildAdvanceTable: unique codepoint cap (%u) hit, layout may be approximate",
            MAX_UNIQUE_CODEPOINTS);
  }
  std::sort(codepoints, codepoints + cpCount);
  int totalMissed = fetchAdvancesForCodepoints(codepoints, cpCount, styleMask);
  delete[] codepoints;
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, false, false, styleMask);
}

int SdCardFont::buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask) {
  return buildAdvanceTableRange(words.begin(), words.end(), words.size() > 1, includeHyphen, styleMask);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  if (!self->ensureStyleIntervalsLoaded(styleIdx)) return nullptr;
  const auto& s = self->styles_[styleIdx];

  // Shared-pool lookup — on hit, find() bumps the LRU timestamp.
  auto& cache = SdFontGlyphCache::getInstance();
  if (const EpdGlyph* hit = cache.findGlyph(self, styleIdx, codepoint)) return hit;

  // Miss — need to load from SD. Look up the global glyph index first; if
  // the codepoint isn't in this font's coverage, bail before touching SD.
  const int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Read through the persistent handle (no per-miss directory-walking open).
  HalFile* file = self->ensureOverflowFileOpen();
  if (!file) return nullptr;

  // Read into temporaries so a mid-load failure leaves the cache untouched.
  EpdGlyph tempGlyph = {};
  const uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file->seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }
  if (file->read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  std::unique_ptr<uint8_t[]> tempBitmap;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[tempGlyph.dataLength]);
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file->seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      return nullptr;
    }
    if (file->read(tempBitmap.get(), tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      return nullptr;
    }
  }

  // All reads succeeded — commit into the shared pool. The returned glyph
  // pointer is consumed (measured/drawn) before the next miss per the
  // EpdFontData::glyphMissHandler contract, so a later insert's realloc is safe.
  return cache.insertGlyph(self, styleIdx, codepoint, tempGlyph, std::move(tempBitmap));
}

const int8_t* SdCardFont::onKernRow(void* ctx, uint8_t leftClass) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  const uint16_t rowWidth = s.header.kernRightClassCount;
  if (leftClass == 0 || leftClass > s.header.kernLeftClassCount || rowWidth == 0) return nullptr;

  // Shared-pool lookup — on hit, find() bumps the LRU timestamp.
  auto& cache = SdFontGlyphCache::getInstance();
  if (const int8_t* hit = cache.findKernRow(self, styleIdx, leftClass)) return hit;

  // Miss — read this left class's matrix row through the persistent handle.
  HalFile* file = self->ensureOverflowFileOpen();
  if (!file) return nullptr;
  const uint32_t rowOff = s.kernMatrixFileOffset + static_cast<uint32_t>(leftClass - 1) * rowWidth;
  if (!file->seekSet(rowOff)) {
    LOG_ERR("SDCF", "Kern: failed to seek to row %u style %u", leftClass, styleIdx);
    return nullptr;
  }
  std::unique_ptr<int8_t[]> tmp(new (std::nothrow) int8_t[rowWidth]);
  if (!tmp) {
    LOG_ERR("SDCF", "Kern: OOM row (%u bytes)", rowWidth);
    return nullptr;
  }
  if (file->read(reinterpret_cast<uint8_t*>(tmp.get()), rowWidth) != static_cast<int>(rowWidth)) {
    LOG_ERR("SDCF", "Kern: short row read (class %u style %u)", leftClass, styleIdx);
    return nullptr;
  }

  // The cached row's buffer is consumed by getKerning before the next call.
  return cache.insertKernRow(self, styleIdx, leftClass, tmp.get(), rowWidth);
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  return SdFontGlyphCache::getInstance().isOverflowGlyph(glyph);
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  return SdFontGlyphCache::getInstance().bitmapFor(glyph);
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
