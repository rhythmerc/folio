#include "ThemeFontManager.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
constexpr char LOG_TAG[] = "THMFNT";

constexpr const char* kRoleNames[] = {
    "title", "heading", "body", "caption", "accent",
    "body-compact", "caption-compact", "accent-compact", "body-large"};

uint32_t fnvHash(const char* str) {
  uint32_t hash = 2166136261u;
  while (*str) {
    hash ^= static_cast<uint8_t>(*str++);
    hash *= 16777619u;
  }
  return hash;
}

int computeBundledFontId(const char* themeId, const char* roleName) {
  uint32_t hash = fnvHash(themeId);
  hash ^= 0x42;
  hash *= 16777619u;
  while (*roleName) {
    hash ^= static_cast<uint8_t>(*roleName++);
    hash *= 16777619u;
  }
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

}  // namespace

ThemeFontManager& ThemeFontManager::getInstance() {
  static ThemeFontManager instance;
  return instance;
}

ThemeFontManager::LoadedFontIds ThemeFontManager::loadRoles(
    const char* themeId, const char* cacheDir, const ThemeFontSpec& fontSpec,
    GfxRenderer& renderer) {
  LoadedFontIds out;
  LOG_INF(LOG_TAG, "loadRoles: heap free=%u, maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  int* idSlots[] = {&out.title,          &out.heading,       &out.body,
                    &out.caption,        &out.accent,        &out.bodyCompact,
                    &out.captionCompact, &out.accentCompact, &out.bodyLarge};

  for (int i = 0; i < kFontRoleCount; ++i) {
    const auto& role = fontSpec.roles[i];

    // Try loading an SD card font file first.
    if (role.file[0] != '\0') {
      char fontPath[128];
      snprintf(fontPath, sizeof(fontPath), "%s/%s", cacheDir, role.file);

      if (Storage.exists(fontPath)) {
        // Reuse an already-loaded SdCardFont if another role pointed at the
        // same file. Each role still gets its own fontId so the renderer can
        // map role → font; only the heavy backing instance is shared.
        SdCardFont* sharedFont = nullptr;
        for (auto& lf : loadedFonts_) {
          if (strcmp(lf.path, fontPath) == 0) {
            sharedFont = lf.font.get();
            break;
          }
        }

        if (!sharedFont) {
          auto font = makeUniqueNoThrow<SdCardFont>();
          if (!font) {
            LOG_ERR(LOG_TAG, "OOM: SdCardFont for %s", fontPath);
          } else if (!font->load(fontPath)) {
            LOG_ERR(LOG_TAG, "Failed to load font %s", fontPath);
          } else {
            LoadedFont entry;
            strncpy(entry.path, fontPath, sizeof(entry.path) - 1);
            entry.path[sizeof(entry.path) - 1] = '\0';
            entry.font = std::move(font);
            sharedFont = entry.font.get();
            loadedFonts_.push_back(std::move(entry));
            LOG_INF(LOG_TAG, "Loaded %s (heap free=%u, maxAlloc=%u)", fontPath,
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          }
        }

        if (sharedFont) {
          const int fontId = computeBundledFontId(themeId, kRoleNames[i]);
          renderer.registerSdCardFont(fontId, sharedFont);
          // Resolve each slot through SdCardFont::resolveStyle() so missing
          // styles are backed by the closest present style instead of nullptr.
          // A .cpfont with only italic present (e.g. Nunito_7) would otherwise
          // hand a null `regular` pointer to EpdFontFamily, and the renderer
          // would deref it the first time any text drew in REGULAR style.
          EpdFontFamily family(sharedFont->getEpdFont(sharedFont->resolveStyle(0)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(1)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(2)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(3)));
          renderer.insertFont(fontId, family);

          *idSlots[i] = fontId;
          registeredFontIds_.push_back(fontId);
          LOG_DBG(LOG_TAG, "Bound role '%s' -> %s (id=%d)", kRoleNames[i], fontPath, fontId);
          continue;
        }
      }
    }

    // Fall back to a builtin font reference.
    if (role.builtin[0] != '\0') {
      const int builtinId = resolveBuiltinFontName(role.builtin);
      if (builtinId != 0) {
        *idSlots[i] = builtinId;
      }
    }
  }

  return out;
}

void ThemeFontManager::clear(GfxRenderer& renderer) {
  for (int fontId : registeredFontIds_) {
    renderer.removeFont(fontId);
  }
  registeredFontIds_.clear();
  loadedFonts_.clear();
}
