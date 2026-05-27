#pragma once

#include <cstddef>
#include <vector>

// Discovers .cptheme archives on the SD card and manages their extraction to
// the /.themes/<id>/ cache directory. Knows nothing about fonts, ThemeData,
// or the renderer — UiThemeLoader owns those concerns.
class CpThemeArchive {
 public:
  struct ThemeInfo {
    char id[32];
    char name[48];
    char cpthemePath[128];
  };

  // Scan /.themes/ and /themes/ for .cptheme files. Replaces the discovered
  // list. Each valid archive contributes one entry (id/name/source path).
  void discoverThemes();

  const std::vector<ThemeInfo>& getDiscoveredThemes() const { return themeList_; }

  // Look up a discovered theme by id. Returns nullptr if unknown.
  const ThemeInfo* findThemeInfo(const char* themeId) const;

  // Ensure the archive for `info` has been extracted to /.themes/<id>/.
  // Returns true if the cache is now valid; extraction is a no-op when the
  // existing cache matches the source file hash.
  bool ensureExtracted(const ThemeInfo& info);

  // Write the cached theme.json path for `themeId` into `outPath`. Does not
  // verify existence — caller should attempt the read and handle failure.
  void getCachedJsonPath(const char* themeId, char* outPath, size_t outSize) const;

  // Write the per-theme cache directory ("/.themes/<id>") into `outPath`.
  // Used by ThemeFontManager to resolve role .cpfont paths.
  void getCacheDir(const char* themeId, char* outPath, size_t outSize) const;

 private:
  std::vector<ThemeInfo> themeList_;

  void scanRoot(const char* rootPath);
  bool extractCptheme(const char* cpthemePath, const char* themeId);
  bool isExtracted(const char* themeId, const char* cpthemePath) const;
};
