#pragma once

#include <memory>
#include <vector>

#include "ThemeJsonParser.h"

class GfxRenderer;
class SdCardFont;

// Owns the SD-card font lifecycle for theme roles: up to 8 SdCardFont
// instances (one per semantic role: title, heading, body, caption, accent,
// plus three compact variants), deduplicated by .cpfont path so multiple
// roles can share a single backing font.
//
// Counterpart on the reader side is ReaderFontSystem; the two are
// intentionally separate because they have different constraints (many
// fonts vs one) and different lifecycles.
class ThemeFontManager {
 public:
  static ThemeFontManager& getInstance();

  // Resolved font IDs after loadRoles(). Zero indicates the role has no
  // backing font (neither SD file nor builtin reference present).
  struct LoadedFontIds {
    int title = 0;
    int heading = 0;
    int body = 0;
    int caption = 0;
    int accent = 0;
    int bodyCompact = 0;
    int captionCompact = 0;
    int accentCompact = 0;
  };

  // Load every role described in `fontSpec` from the extracted cache dir.
  // For each role, registers an EpdFontFamily with the renderer and returns
  // the resolved font ID. Roles with a builtin reference fall back to the
  // compiled-in font when the SD path is absent.
  LoadedFontIds loadRoles(const char* themeId, const char* cacheDir,
                          const ThemeFontSpec& fontSpec, GfxRenderer& renderer);

  // Free all loaded SD fonts and unregister them from the renderer.
  void clear(GfxRenderer& renderer);

 private:
  ThemeFontManager() = default;
  ~ThemeFontManager() = default;
  ThemeFontManager(const ThemeFontManager&) = delete;
  ThemeFontManager& operator=(const ThemeFontManager&) = delete;

  // One LoadedFont per unique .cpfont path. Multiple theme roles that point
  // at the same file share a single SdCardFont — each role still gets its
  // own fontId registered with the renderer (see registeredFontIds_).
  // This dedup keeps theme RAM bounded when every role bundles an SD font:
  // e.g. RoundedRaff has 8 roles backed by 4 unique files, so dedup roughly
  // halves the resident font footprint.
  struct LoadedFont {
    char path[128];
    std::unique_ptr<SdCardFont> font;
  };
  std::vector<LoadedFont> loadedFonts_;

  // Every fontId we asked the renderer to register, in registration order.
  // clear() walks this to unregister; loadedFonts_ owns the SdCardFonts.
  std::vector<int> registeredFontIds_;
};
