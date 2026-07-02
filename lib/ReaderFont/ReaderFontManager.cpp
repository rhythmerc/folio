#include "ReaderFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <ReaderFontRegistry.h>
#include <SdCardFont.h>

ReaderFontManager::~ReaderFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int ReaderFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

bool ReaderFontManager::bindFamily(const ReaderFontFamilyInfo& family, GfxRenderer& renderer) {
  unloadAll(renderer);

  loaded_.reserve(family.files.size());
  for (const auto& f : family.files) {
    auto* font = new (std::nothrow) SdCardFont();
    if (!font) {
      LOG_ERR("RDRMGR", "OOM SdCardFont for %s", f.path.c_str());
      continue;
    }
    if (!font->load(f.path.c_str())) {
      LOG_ERR("RDRMGR", "Failed to load %s", f.path.c_str());
      delete font;
      continue;
    }

    const int fontId = computeFontId(font->contentHash(), family.name.c_str(), f.pointSize);
    // Guard against colliding with an already-registered id (built-in / other).
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("RDRMGR", "Font id %d collides, skipping %s", fontId, f.path.c_str());
      delete font;
      continue;
    }

    renderer.registerSdCardFont(fontId, font);
    EpdFontFamily fontFamily(font->getEpdFont(font->resolveStyle(0)), font->getEpdFont(font->resolveStyle(1)),
                             font->getEpdFont(font->resolveStyle(2)), font->getEpdFont(font->resolveStyle(3)));
    renderer.insertFont(fontId, fontFamily);
    loaded_.push_back({font, fontId, f.pointSize});
  }

  if (loaded_.empty()) {
    LOG_ERR("RDRMGR", "Family %s: no sizes loaded", family.name.c_str());
    return false;
  }
  loadedFamilyName_ = family.name;
  LOG_DBG("RDRMGR", "Bound %s (%zu sizes)", family.name.c_str(), loaded_.size());
  return true;
}

void ReaderFontManager::unloadAll(GfxRenderer& renderer) {
  // removeFont() erases both the fontMap and sdCardFonts_ entries for this id,
  // so we touch only our own ids and leave any theme UI SD fonts alone.
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
}

const ReaderFontManager::LoadedFont* ReaderFontManager::snapSize(uint8_t pt) const {
  const LoadedFont* best = nullptr;
  int bestDelta = 0;
  for (const auto& lf : loaded_) {
    const int d = (lf.size > pt) ? (lf.size - pt) : (pt - lf.size);
    if (!best || d < bestDelta || (d == bestDelta && lf.size > best->size)) {  // ties -> larger size
      best = &lf;
      bestDelta = d;
    }
  }
  return best;
}

int ReaderFontManager::idForSize(const char* familyName, uint8_t pt) const {
  if (loadedFamilyName_.empty() || loadedFamilyName_ != familyName) return 0;
  const LoadedFont* lf = snapSize(pt);
  return lf ? lf->fontId : 0;
}
