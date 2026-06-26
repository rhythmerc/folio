#pragma once

#include <cstddef>

#include "components/themes/BaseTheme.h"

struct ThemeData;

static constexpr int kFontRoleCount = 9;

struct ThemeFontSpec {
  struct RoleSpec {
    char file[64];
    char builtin[24];
  };
  RoleSpec roles[kFontRoleCount];
};

// Parse a theme.json blob into `out`, starting from Folio defaults.
// Only fields present in the JSON are overwritten; the rest inherit.
// `idBuf` and `nameBuf` receive the theme id and display name (required
// fields — returns false if either is missing).
// `fontSpec` is populated with per-role font file/builtin references.
bool parseThemeJson(const char* json, size_t len, ThemeData& out,
                    char* idBuf, size_t idBufSize,
                    char* nameBuf, size_t nameBufSize,
                    ThemeFontSpec& fontSpec);

// Resolve a builtin font name (e.g. "ui-12", "notoserif-14") to its
// compiled-in font ID. Returns 0 if the name is unrecognized.
int resolveBuiltinFontName(const char* name);
