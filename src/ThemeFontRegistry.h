#pragma once

#include <memory>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"  // for FontRole

class GfxRenderer;
class SdCardFont;

// Theme font role progressive enhancement.
//
// Themes ask for fonts by semantic role (Title/Heading/Body/Caption/Accent).
// At boot, this registry walks the SD card looking for role-specific
// `.cpfont` files at
//
//   /.fonts/themes/<themeName>/<roleName>.cpfont
//
// Each file found is loaded, registered with the renderer (so drawText sees
// it as a normal font ID), and indexed under (themeName, role) for lookup.
//
// Themes that want SD progressive enhancement consult `getRoleFont()` in
// their `getFontForRole()` override and fall back to embedded faces when no
// SD override is installed.
//
// Each loaded SD role font carries the usual SdCardFont cost (~10–30 KB of
// kern/intervals/cache per face). With at most one face per role and
// typically 1–2 roles overridden per theme, the steady-state cost stays
// within budget. The registry never auto-loads — it only loads files the
// user has explicitly placed on the SD card.
class ThemeFontRegistry {
 public:
  static ThemeFontRegistry& getInstance();

  // Discover all `/.fonts/themes/*/*.cpfont` files, load them, and register
  // with the renderer. Safe to call exactly once during boot (after
  // Storage.begin()). Subsequent calls reload the registry from disk —
  // useful after the web UI installs/removes a font.
  void discover(GfxRenderer& renderer);

  // Returns the renderer font ID registered for (themeName, role), or 0
  // when no SD override is installed. The 0 sentinel matches the renderer's
  // "font not found" convention.
  int getRoleFont(const char* themeName, FontRole role) const;

 private:
  struct LoadedRoleFont {
    std::string themeName;
    FontRole role;
    int fontId = 0;
    std::unique_ptr<SdCardFont> font;
  };

  std::vector<LoadedRoleFont> loaded_;

  ThemeFontRegistry() = default;
  ~ThemeFontRegistry();
  ThemeFontRegistry(const ThemeFontRegistry&) = delete;
  ThemeFontRegistry& operator=(const ThemeFontRegistry&) = delete;

  // Unregister all loaded role fonts from the renderer and drop them.
  void clear(GfxRenderer& renderer);

  // Try to load one file under (themeName, roleName). Returns true on success.
  bool loadRoleFile(GfxRenderer& renderer, const std::string& themeName, const std::string& roleName,
                    const std::string& filePath);

  // Walk one root (`/.fonts` or `/fonts`) for theme role files.
  void scanRoot(GfxRenderer& renderer, const char* rootPath);

  // Parse "roleName" out of a filename like "caption.cpfont". Returns
  // FontRole and sets ok=true on success.
  static FontRole parseRoleName(const char* filename, bool& ok);

  // Generate a deterministic font ID for (themeName, role, contentHash).
  // Same FNV-1a continuation pattern used by SdCardFontManager.
  static int computeFontId(uint32_t contentHash, const char* themeName, const char* roleName);
};

#define THEME_FONTS ThemeFontRegistry::getInstance()
