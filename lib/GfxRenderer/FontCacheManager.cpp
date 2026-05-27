#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
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

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  scanText_ += text;
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  scanStyleCounts_[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager, bool persistent)
    : manager_(&manager), persistent_(persistent) {
  manager_->scanMode_ = ScanMode::Scanning;
  // Destructive mode wipes the existing mini cache up front so the rebuild
  // is the only resident state. Persistent mode preserves it — the
  // idempotent prewarm short-circuit in SdCardFont compares the new request
  // against the prior hash and skips rebuild when stable.
  if (!persistent_) {
    manager_->clearCache();
  }
  manager_->resetStats();
  manager_->scanText_.clear();
  manager_->scanText_.reserve(2048);
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanText_.empty()) return;

  // Build style bitmask from all styles that appeared during the scan
  uint8_t styleMask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanStyleCounts_[i] > 0) styleMask |= (1 << i);
  }
  if (styleMask == 0) styleMask = 1;  // default to regular

  manager_->prewarmCache(manager_->scanFontId_, manager_->scanText_.c_str(), styleMask);

  // Free scan string memory
  manager_->scanText_.clear();
  manager_->scanText_.shrink_to_fit();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanText_ is empty)
    if (!persistent_) {
      manager_->clearCache();
    }
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_), persistent_(other.persistent_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() {
  return PrewarmScope(*this, /*persistent=*/false);
}

FontCacheManager::PrewarmScope FontCacheManager::createPersistentPrewarmScope() {
  return PrewarmScope(*this, /*persistent=*/true);
}

// --- TextCollector implementation ---

void TextCollector::use(int fontId, EpdFontFamily::Style style, const char* text) {
  if (fontId == 0 || text == nullptr || text[0] == '\0') return;
  auto& entry = byFont_[fontId];
  entry.styleMask |= static_cast<uint8_t>(1u << static_cast<uint8_t>(style));
  entry.text += text;
}

void TextCollector::applyTo(FontCacheManager& fcm) const {
  // Dedup by underlying SdCardFont — multiple theme roles can be backed by
  // the same SdCardFont when their .cpfont files match (UiThemeLoader
  // collapses identical paths). The mini glyph cache inside SdCardFont is
  // destructive on rebuild, so two prewarms with different text would have
  // the second clobber the first — every glyph not in the last prewarm
  // would then fall through to per-glyph SD reads during drawText. Merge
  // text + styleMask per shared instance and prewarm once.
  std::map<SdCardFont*, PerFont> bySdFont;
  for (const auto& [fontId, entry] : byFont_) {
    if (entry.styleMask == 0 || entry.text.empty()) continue;
    SdCardFont* sdFont = fcm.findSdCardFont(fontId);
    if (!sdFont) {
      // Non-SD font (compressed builtin): each fontId has its own cache,
      // so no sharing to worry about — prewarm directly.
      fcm.prewarmCache(fontId, entry.text.c_str(), entry.styleMask);
      continue;
    }
    auto& slot = bySdFont[sdFont];
    slot.text += entry.text;
    slot.styleMask |= entry.styleMask;
  }
  for (const auto& [sdFont, merged] : bySdFont) {
    int missed = sdFont->prewarm(merged.text.c_str(), merged.styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD,merged): %d glyph(s) not found (styleMask=0x%02X)", missed, merged.styleMask);
    }
  }
}
