#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct ReaderFontFamilyInfo;

// Loads the active reader SD font family for the reader.
//
// A family ships several .cpfont files (one per point size). Like the UI side
// (ThemeFontManager::loadRoles), bindFamily EAGERLY loads + registers every
// shipped size once. This is cheap because SdCardFont is lazy: a loaded size
// only costs its header + per-style stubs (~1 KB) until actually rendered —
// intervals AND kern/ligature tables populate on first use (see
// SdCardFont::ensureStyleIntervalsLoaded), and glyph bitmaps live in the shared
// SdFontGlyphCache budget. So a size that never appears on screen stays nearly
// free, and no per-page loading, eviction, or pinning is needed.
//
// Font ids are deterministic (FNV-1a of contentHash + family + pointSize), so an
// id computed at layout (and baked into the section cache) matches the id the
// size is registered under — stable across reboots.
//
// NOT thread-safe; all SD font work is on the render task (matches SdCardFont).
class ReaderFontManager {
 public:
  ReaderFontManager() = default;
  ~ReaderFontManager();
  ReaderFontManager(const ReaderFontManager&) = delete;
  ReaderFontManager& operator=(const ReaderFontManager&) = delete;

  // Bind `family`: unload the previous family, then load + register every
  // available size. Returns false if no size could be loaded.
  bool bindFamily(const ReaderFontFamilyInfo& family, GfxRenderer& renderer);

  // Unload every size and forget the bound family.
  void unloadAll(GfxRenderer& renderer);

  // Deterministic id for the loaded size nearest `pt` in the bound family, or 0
  // if `familyName` isn't the bound family / nothing bound. Pure lookup — every
  // size is already loaded + registered, so the returned id is renderable.
  int idForSize(const char* familyName, uint8_t pt) const;

  const std::string& currentFamilyName() const { return loadedFamilyName_; }
  bool isBound() const { return !loaded_.empty(); }

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };

  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);
  const LoadedFont* snapSize(uint8_t pt) const;  // nearest loaded size (ties -> larger)

  std::string loadedFamilyName_;
  std::vector<LoadedFont> loaded_;  // one per shipped size of the bound family
};
