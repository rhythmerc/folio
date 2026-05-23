#pragma once

#include <memory>
#include <vector>

#include "ThemeJsonParser.h"
#include "components/themes/ThemeData.h"

class GfxRenderer;
class SdCardFont;

class SdThemeLoader {
 public:
  static SdThemeLoader& getInstance();

  struct ThemeInfo {
    char id[32];
    char name[48];
    char cpthemePath[128];
  };

  // Scan /.themes/ and /themes/ for .cptheme files. Populates the
  // discovered-themes list with id/name/path for each valid archive.
  void discoverThemes();

  // Load a theme by id. Extracts the .cptheme archive if not already
  // cached, parses theme.json, loads bundled fonts. Returns false on
  // failure (falls back to Folio in the caller).
  bool loadTheme(const char* themeId, GfxRenderer& renderer);

  // Unload the current SD theme: unregister and free fonts, release
  // the heap-allocated ThemeData.
  void unloadTheme(GfxRenderer& renderer);

  const ThemeData* getData() const { return themeData_.get(); }
  bool isLoaded() const { return themeData_ != nullptr; }
  const char* getLoadedId() const { return isLoaded() ? idBuf_ : ""; }

  const std::vector<ThemeInfo>& getDiscoveredThemes() const { return themeList_; }

 private:
  SdThemeLoader() = default;
  ~SdThemeLoader();
  SdThemeLoader(const SdThemeLoader&) = delete;
  SdThemeLoader& operator=(const SdThemeLoader&) = delete;

  std::unique_ptr<ThemeData> themeData_;
  char idBuf_[32] = "";
  char nameBuf_[48] = "";
  std::vector<ThemeInfo> themeList_;

  struct LoadedFont {
    int fontId;
    std::unique_ptr<SdCardFont> font;
  };
  std::vector<LoadedFont> loadedFonts_;

  // Scan one root directory for .cptheme files.
  void scanRoot(const char* rootPath);

  // Extract a .cptheme archive to /.themes/<id>/ cache directory.
  bool extractCptheme(const char* cpthemePath, const char* themeId);

  // Check whether an extraction cache exists for the given theme.
  bool isExtracted(const char* themeId) const;

  // Load theme fonts from the extracted cache using the parsed font spec.
  void loadThemeFonts(const char* themeId, const ThemeFontSpec& fontSpec,
                      GfxRenderer& renderer);

  // Free all loaded SD fonts and unregister them from the renderer.
  void clearFonts(GfxRenderer& renderer);

  // Find the .cptheme path for a given theme id in the discovered list.
  const ThemeInfo* findThemeInfo(const char* themeId) const;
};

#define SD_THEMES SdThemeLoader::getInstance()
