#include "ThemeJsonParser.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstring>

#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.h"
#include "fontIds.h"

void parseThemeFields(JsonDocument& doc, ThemeData& out);

namespace {

// ─── Builtin font name → ID lookup ─────────────────────────────────

struct BuiltinFontEntry {
  const char* name;
  int id;
};

constexpr BuiltinFontEntry kBuiltinFonts[] = {
    {"notoserif-5", NOTOSERIF_5_FONT_ID},
    {"notoserif-6", NOTOSERIF_6_FONT_ID},
    {"notoserif-8", NOTOSERIF_8_FONT_ID},
    {"notoserif-10", NOTOSERIF_10_FONT_ID},
    {"notoserif-12", NOTOSERIF_12_FONT_ID},
    {"notoserif-14", NOTOSERIF_14_FONT_ID},
    {"notoserif-16", NOTOSERIF_16_FONT_ID},
    {"notoserif-18", NOTOSERIF_18_FONT_ID},
    {"notosans-12", NOTOSANS_12_FONT_ID},
    {"notosans-14", NOTOSANS_14_FONT_ID},
    {"notosans-16", NOTOSANS_16_FONT_ID},
    {"notosans-18", NOTOSANS_18_FONT_ID},
    {"ui-10", UI_10_FONT_ID},
    {"ui-12", UI_12_FONT_ID},
    {"small", SMALL_FONT_ID},
};

// ─── Font spec parsing ─────────────────────────────────────────────

const char* kRoleKeys[] = {"title", "heading", "body", "caption", "accent",
                           "body-compact", "caption-compact", "accent-compact"};

void parseFontSpecs(JsonObjectConst fonts, ThemeFontSpec& spec) {
  memset(&spec, 0, sizeof(spec));
  for (int i = 0; i < kFontRoleCount; ++i) {
    JsonObjectConst role = fonts[kRoleKeys[i]];
    if (role.isNull()) continue;
    const char* file = role["file"];
    if (file) {
      strncpy(spec.roles[i].file, file, sizeof(spec.roles[i].file) - 1);
      spec.roles[i].file[sizeof(spec.roles[i].file) - 1] = '\0';
    }
    const char* builtin = role["builtin"];
    if (builtin) {
      strncpy(spec.roles[i].builtin, builtin, sizeof(spec.roles[i].builtin) - 1);
      spec.roles[i].builtin[sizeof(spec.roles[i].builtin) - 1] = '\0';
    }
  }
}

}  // namespace

// ─── Public API ─────────────────────────────────────────────────────

int resolveBuiltinFontName(const char* name) {
  if (!name || name[0] == '\0') return 0;
  for (const auto& e : kBuiltinFonts) {
    if (strcmp(e.name, name) == 0) return e.id;
  }
  return 0;
}

bool parseThemeJson(const char* json, size_t len, ThemeData& out,
                    char* idBuf, size_t idBufSize,
                    char* nameBuf, size_t nameBufSize,
                    ThemeFontSpec& fontSpec) {
  // Start from Folio defaults.
  out = BuiltinThemes::Folio;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    LOG_ERR("TPARSE", "JSON parse error: %s", err.c_str());
    return false;
  }

  // Required fields.
  const char* id = doc["id"];
  const char* name = doc["name"];
  if (!id || id[0] == '\0') {
    LOG_ERR("TPARSE", "Missing required field: id");
    return false;
  }
  if (!name || name[0] == '\0') {
    LOG_ERR("TPARSE", "Missing required field: name");
    return false;
  }
  strncpy(idBuf, id, idBufSize - 1);
  idBuf[idBufSize - 1] = '\0';
  strncpy(nameBuf, name, nameBufSize - 1);
  nameBuf[nameBufSize - 1] = '\0';
  out.id = idBuf;

  parseThemeFields(doc, out);

  // ─── Fonts ──────────────────────────────────────────────────────
  JsonObjectConst fonts = doc["fonts"];
  if (!fonts.isNull()) {
    parseFontSpecs(fonts, fontSpec);
  } else {
    memset(&fontSpec, 0, sizeof(fontSpec));
  }

  return true;
}
