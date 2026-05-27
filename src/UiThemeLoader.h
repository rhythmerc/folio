#pragma once

#include <memory>
#include <vector>

#include "CpThemeArchive.h"

class GfxRenderer;
struct ThemeData;

// Orchestrates SD theme loading: owns the active ThemeData (colors,
// dimensions, layout), delegates archive discovery/extraction to
// CpThemeArchive, and delegates SD font lifecycle to ThemeFontManager.
//
// The font miss handler used for lazy theme-font restoration lives on
// ThemeFontManager — wire it via ThemeFontManager::onFontMiss in main.cpp.
class UiThemeLoader {
 public:
  static UiThemeLoader& getInstance();

  // Re-export the archive's ThemeInfo as a nested alias so existing callers
  // (e.g. SettingsList) keep their UiThemeLoader::ThemeInfo references valid.
  using ThemeInfo = CpThemeArchive::ThemeInfo;

  // Scan /.themes/ and /themes/ for .cptheme files.
  void discoverThemes();

  // Load a theme by id. Ensures the archive is extracted, parses theme.json,
  // and loads bundled fonts. Returns false on failure (caller falls back to
  // Folio).
  bool loadTheme(const char* themeId, GfxRenderer& renderer);

  // Unload the current SD theme: free fonts and release ThemeData.
  void unloadTheme(GfxRenderer& renderer);

  // Free every SD card font without unloading the theme. Per-role font IDs
  // are remembered for lazy restoration; the theme metadata (colors,
  // dimensions, layout) stays resident. Used by the EPUB reader, which
  // touches zero theme roles, to reclaim heap so mid-session chunked
  // allocations (e.g. the BW buffer snapshot for grayscale rendering) don't
  // fail under heap fragmentation.
  void evictFonts(GfxRenderer& renderer);

  const ThemeData* getData() const { return themeData_.get(); }
  bool isLoaded() const { return themeData_ != nullptr; }
  const char* getLoadedId() const { return isLoaded() ? idBuf_ : ""; }

  const std::vector<ThemeInfo>& getDiscoveredThemes() const {
    return archive_.getDiscoveredThemes();
  }

 private:
  UiThemeLoader() = default;
  ~UiThemeLoader();
  UiThemeLoader(const UiThemeLoader&) = delete;
  UiThemeLoader& operator=(const UiThemeLoader&) = delete;

  CpThemeArchive archive_;
  std::unique_ptr<ThemeData> themeData_;
  char idBuf_[32] = "";
  char nameBuf_[48] = "";
};

#define UI_THEMES UiThemeLoader::getInstance()
