#include "ThemeJsonParser.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstring>

#include "components/themes/ThemeData.h"
#include "fontIds.h"

namespace {

// ─── Enum string ↔ value mappings ──────────────────────────────────

struct EnumEntry {
  const char* name;
  uint8_t value;
};

template <typename E, size_t N>
E parseEnum(const char* str, const EnumEntry (&table)[N], E fallback) {
  if (!str) return fallback;
  for (const auto& e : table) {
    if (strcmp(str, e.name) == 0) return static_cast<E>(e.value);
  }
  return fallback;
}

constexpr EnumEntry kHeaderStyles[] = {
    {"centered-title", static_cast<uint8_t>(HeaderStyle::CenteredTitle)},
    {"left-aligned-with-rule", static_cast<uint8_t>(HeaderStyle::LeftAlignedWithRule)},
    {"left-aligned-bordered", static_cast<uint8_t>(HeaderStyle::LeftAlignedBordered)},
    {"left-aligned-plain", static_cast<uint8_t>(HeaderStyle::LeftAlignedPlain)},
};

constexpr EnumEntry kSelectionStyles[] = {
    {"solid-fill", static_cast<uint8_t>(SelectionStyle::SolidFill)},
    {"rounded-fill", static_cast<uint8_t>(SelectionStyle::RoundedFill)},
    {"rounded-row-always", static_cast<uint8_t>(SelectionStyle::RoundedRowAlways)},
    {"layered-frame", static_cast<uint8_t>(SelectionStyle::LayeredFrame)},
};

constexpr EnumEntry kCoverStyles[] = {
    {"default", static_cast<uint8_t>(CoverStyle::Default)},
    {"card", static_cast<uint8_t>(CoverStyle::Card)},
    {"card-rounded", static_cast<uint8_t>(CoverStyle::CardRounded)},
};

constexpr EnumEntry kButtonHintsStyles[] = {
    {"boxed", static_cast<uint8_t>(ButtonHintsStyle::Boxed)},
    {"hairline", static_cast<uint8_t>(ButtonHintsStyle::Hairline)},
    {"rounded-filled", static_cast<uint8_t>(ButtonHintsStyle::RoundedFilled)},
    {"paired-groups", static_cast<uint8_t>(ButtonHintsStyle::PairedGroups)},
};

constexpr EnumEntry kSideButtonHintsStyles[] = {
    {"sharp", static_cast<uint8_t>(SideButtonHintsStyle::Sharp)},
    {"rounded", static_cast<uint8_t>(SideButtonHintsStyle::Rounded)},
};

constexpr EnumEntry kTabBarStyles[] = {
    {"underline", static_cast<uint8_t>(TabBarStyle::Underline)},
    {"dithered-rounded", static_cast<uint8_t>(TabBarStyle::DitheredRounded)},
    {"slot-rounded", static_cast<uint8_t>(TabBarStyle::SlotRounded)},
};

constexpr EnumEntry kScrollIndicatorStyles[] = {
    {"arrows", static_cast<uint8_t>(ScrollIndicatorStyle::Arrows)},
    {"thumb", static_cast<uint8_t>(ScrollIndicatorStyle::Thumb)},
    {"line-thumb", static_cast<uint8_t>(ScrollIndicatorStyle::LineThumb)},
};

constexpr EnumEntry kBatteryStyles[] = {
    {"solid", static_cast<uint8_t>(BatteryStyle::Solid)},
    {"segmented", static_cast<uint8_t>(BatteryStyle::Segmented)},
};

constexpr EnumEntry kFontStyles[] = {
    {"regular", static_cast<uint8_t>(EpdFontFamily::REGULAR)},
    {"bold", static_cast<uint8_t>(EpdFontFamily::BOLD)},
    {"italic", static_cast<uint8_t>(EpdFontFamily::ITALIC)},
    {"bold-italic", static_cast<uint8_t>(EpdFontFamily::BOLD_ITALIC)},
};

EpdFontFamily::Style parseFontStyle(const char* str, EpdFontFamily::Style fallback) {
  return parseEnum(str, kFontStyles, fallback);
}

// ─── Builtin font name → ID lookup ─────────────────────────────────

struct BuiltinFontEntry {
  const char* name;
  int id;
};

constexpr BuiltinFontEntry kBuiltinFonts[] = {
    {"notoserif-10", NOTOSERIF_10_FONT_ID},
    {"notoserif-12", NOTOSERIF_12_FONT_ID},
    {"notoserif-14", NOTOSERIF_14_FONT_ID},
    {"notoserif-16", NOTOSERIF_16_FONT_ID},
    {"notoserif-18", NOTOSERIF_18_FONT_ID},
    {"notosans-12", NOTOSANS_12_FONT_ID},
    {"notosans-14", NOTOSANS_14_FONT_ID},
    {"notosans-16", NOTOSANS_16_FONT_ID},
    {"notosans-18", NOTOSANS_18_FONT_ID},
    {"opendyslexic-8", OPENDYSLEXIC_8_FONT_ID},
    {"opendyslexic-10", OPENDYSLEXIC_10_FONT_ID},
    {"opendyslexic-12", OPENDYSLEXIC_12_FONT_ID},
    {"opendyslexic-14", OPENDYSLEXIC_14_FONT_ID},
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

  // ─── Header ─────────────────────────────────────────────────────
  JsonObjectConst hdr = doc["header"];
  if (!hdr.isNull()) {
    out.header.style = parseEnum(hdr["style"] | static_cast<const char*>(nullptr),
                                 kHeaderStyles, out.header.style);
    out.header.titleStyle = parseFontStyle(hdr["titleStyle"], out.header.titleStyle);
    out.header.subtitleStyle = parseFontStyle(hdr["subtitleStyle"], out.header.subtitleStyle);
    out.header.bottomBorderHeight = hdr["bottomBorderHeight"] | out.header.bottomBorderHeight;
    out.header.height = hdr["height"] | out.header.height;
  }

  // ─── Selection ──────────────────────────────────────────────────
  JsonObjectConst sel = doc["selection"];
  if (!sel.isNull()) {
    out.selection.style = parseEnum(sel["style"] | static_cast<const char*>(nullptr),
                                    kSelectionStyles, out.selection.style);
    out.selection.cornerRadius = sel["cornerRadius"] | out.selection.cornerRadius;
    out.selection.textInverted = sel["textInverted"] | out.selection.textInverted;
  }

  // ─── Cover ──────────────────────────────────────────────────────
  JsonObjectConst cov = doc["cover"];
  if (!cov.isNull()) {
    out.cover.style = parseEnum(cov["style"] | static_cast<const char*>(nullptr),
                                kCoverStyles, out.cover.style);
    out.cover.cornerRadius = cov["cornerRadius"] | out.cover.cornerRadius;
  }

  // ─── List ───────────────────────────────────────────────────────
  JsonObjectConst lst = doc["list"];
  if (!lst.isNull()) {
    out.list.subtitleStyle = parseFontStyle(lst["subtitleStyle"], out.list.subtitleStyle);
    out.list.buttonMenuLabelStyle = parseFontStyle(lst["buttonMenuLabelStyle"], out.list.buttonMenuLabelStyle);
    out.list.rowHeight = lst["rowHeight"] | out.list.rowHeight;
    out.list.rowHeightWithSubtitle = lst["rowHeightWithSubtitle"] | out.list.rowHeightWithSubtitle;
    out.list.menuRowHeight = lst["menuRowHeight"] | out.list.menuRowHeight;
    out.list.menuSpacing = lst["menuSpacing"] | out.list.menuSpacing;
  }

  // ─── Top-level style enums ──────────────────────────────────────
  out.buttonHintsStyle = parseEnum(doc["buttonHintsStyle"] | static_cast<const char*>(nullptr),
                                   kButtonHintsStyles, out.buttonHintsStyle);
  out.sideButtonHintsStyle = parseEnum(doc["sideButtonHintsStyle"] | static_cast<const char*>(nullptr),
                                       kSideButtonHintsStyles, out.sideButtonHintsStyle);
  out.tabBarStyle = parseEnum(doc["tabBarStyle"] | static_cast<const char*>(nullptr),
                              kTabBarStyles, out.tabBarStyle);
  out.scrollIndicatorStyle = parseEnum(doc["scrollIndicatorStyle"] | static_cast<const char*>(nullptr),
                                       kScrollIndicatorStyles, out.scrollIndicatorStyle);
  out.batteryStyle = parseEnum(doc["batteryStyle"] | static_cast<const char*>(nullptr),
                               kBatteryStyles, out.batteryStyle);
  out.showsFileIcons = doc["showsFileIcons"] | out.showsFileIcons;

  // ─── Layout ─────────────────────────────────────────────────────
  JsonObjectConst lay = doc["layout"];
  if (!lay.isNull()) {
    out.layout.topPadding = lay["topPadding"] | out.layout.topPadding;
    out.layout.verticalSpacing = lay["verticalSpacing"] | out.layout.verticalSpacing;
    out.layout.contentSidePadding = lay["contentSidePadding"] | out.layout.contentSidePadding;
  }

  // ─── Battery ────────────────────────────────────────────────────
  JsonObjectConst bat = doc["battery"];
  if (!bat.isNull()) {
    out.battery.width = bat["width"] | out.battery.width;
    out.battery.height = bat["height"] | out.battery.height;
    out.battery.barHeight = bat["barHeight"] | out.battery.barHeight;
  }

  // ─── ButtonHints ────────────────────────────────────────────────
  JsonObjectConst bh = doc["buttonHints"];
  if (!bh.isNull()) {
    out.buttonHints.height = bh["height"] | out.buttonHints.height;
    out.buttonHints.sideWidth = bh["sideWidth"] | out.buttonHints.sideWidth;
  }

  // ─── TabBar ─────────────────────────────────────────────────────
  JsonObjectConst tb = doc["tabBar"];
  if (!tb.isNull()) {
    out.tabBar.spacing = tb["spacing"] | out.tabBar.spacing;
    out.tabBar.height = tb["height"] | out.tabBar.height;
  }

  // ─── ScrollBar ──────────────────────────────────────────────────
  JsonObjectConst sb = doc["scrollBar"];
  if (!sb.isNull()) {
    out.scrollBar.width = sb["width"] | out.scrollBar.width;
    out.scrollBar.rightOffset = sb["rightOffset"] | out.scrollBar.rightOffset;
  }

  // ─── Home ───────────────────────────────────────────────────────
  JsonObjectConst hm = doc["home"];
  if (!hm.isNull()) {
    out.home.topPadding = hm["topPadding"] | out.home.topPadding;
    out.home.coverHeight = hm["coverHeight"] | out.home.coverHeight;
    out.home.coverTileHeight = hm["coverTileHeight"] | out.home.coverTileHeight;
    out.home.recentBooksCount = hm["recentBooksCount"] | out.home.recentBooksCount;
    out.home.continueReadingInMenu = hm["continueReadingInMenu"] | out.home.continueReadingInMenu;
    out.home.menuTopOffset = hm["menuTopOffset"] | out.home.menuTopOffset;
  }

  // ─── ProgressBar ────────────────────────────────────────────────
  JsonObjectConst pb = doc["progressBar"];
  if (!pb.isNull()) {
    out.progressBar.height = pb["height"] | out.progressBar.height;
    out.progressBar.marginTop = pb["marginTop"] | out.progressBar.marginTop;
  }

  // ─── StatusBar ──────────────────────────────────────────────────
  JsonObjectConst stb = doc["statusBar"];
  if (!stb.isNull()) {
    out.statusBar.horizontalMargin = stb["horizontalMargin"] | out.statusBar.horizontalMargin;
    out.statusBar.verticalMargin = stb["verticalMargin"] | out.statusBar.verticalMargin;
  }

  // ─── Keyboard ───────────────────────────────────────────────────
  JsonObjectConst kb = doc["keyboard"];
  if (!kb.isNull()) {
    out.keyboard.keyWidth = kb["keyWidth"] | out.keyboard.keyWidth;
    out.keyboard.keyHeight = kb["keyHeight"] | out.keyboard.keyHeight;
    out.keyboard.keySpacing = kb["keySpacing"] | out.keyboard.keySpacing;
    out.keyboard.bottomKeyHeight = kb["bottomKeyHeight"] | out.keyboard.bottomKeyHeight;
    out.keyboard.bottomKeySpacing = kb["bottomKeySpacing"] | out.keyboard.bottomKeySpacing;
    out.keyboard.bottomAligned = kb["bottomAligned"] | out.keyboard.bottomAligned;
    out.keyboard.centeredText = kb["centeredText"] | out.keyboard.centeredText;
    out.keyboard.verticalOffset = kb["verticalOffset"] | out.keyboard.verticalOffset;
    out.keyboard.textFieldWidthPercent = kb["textFieldWidthPercent"] | out.keyboard.textFieldWidthPercent;
    out.keyboard.widthPercent = kb["widthPercent"] | out.keyboard.widthPercent;
    out.keyboard.keyCornerRadius = kb["keyCornerRadius"] | out.keyboard.keyCornerRadius;
    out.keyboard.fillUnselected = kb["fillUnselected"] | out.keyboard.fillUnselected;
    out.keyboard.outlineAllUnselected = kb["outlineAllUnselected"] | out.keyboard.outlineAllUnselected;
    out.keyboard.drawSpecialOutlineWhenUnselected =
        kb["drawSpecialOutlineWhenUnselected"] | out.keyboard.drawSpecialOutlineWhenUnselected;
    out.keyboard.secondaryLabelRightPadding = kb["secondaryLabelRightPadding"] | out.keyboard.secondaryLabelRightPadding;
    out.keyboard.secondaryLabelTopPadding = kb["secondaryLabelTopPadding"] | out.keyboard.secondaryLabelTopPadding;
    out.keyboard.minArrowHeadSize = kb["minArrowHeadSize"] | out.keyboard.minArrowHeadSize;
  }

  // ─── Popup ──────────────────────────────────────────────────────
  JsonObjectConst pp = doc["popup"];
  if (!pp.isNull()) {
    out.popup.topOffsetRatio = pp["topOffsetRatio"] | out.popup.topOffsetRatio;
    out.popup.marginX = pp["marginX"] | out.popup.marginX;
    out.popup.marginY = pp["marginY"] | out.popup.marginY;
    out.popup.frameThickness = pp["frameThickness"] | out.popup.frameThickness;
    out.popup.cornerRadius = pp["cornerRadius"] | out.popup.cornerRadius;
    out.popup.textBold = pp["textBold"] | out.popup.textBold;
    out.popup.textInverted = pp["textInverted"] | out.popup.textInverted;
    out.popup.textBaselineOffsetY = pp["textBaselineOffsetY"] | out.popup.textBaselineOffsetY;
    JsonObjectConst prog = pp["progress"];
    if (!prog.isNull()) {
      out.popup.progress.barHeight = prog["barHeight"] | out.popup.progress.barHeight;
      out.popup.progress.drawOutline = prog["drawOutline"] | out.popup.progress.drawOutline;
      out.popup.progress.clampPercent = prog["clampPercent"] | out.popup.progress.clampPercent;
      out.popup.progress.fillInverted = prog["fillInverted"] | out.popup.progress.fillInverted;
      out.popup.progress.outlineInverted = prog["outlineInverted"] | out.popup.progress.outlineInverted;
    }
  }

  // ─── TextField ──────────────────────────────────────────────────
  JsonObjectConst tf = doc["textField"];
  if (!tf.isNull()) {
    out.textField.horizontalPadding = tf["horizontalPadding"] | out.textField.horizontalPadding;
    out.textField.normalThickness = tf["normalThickness"] | out.textField.normalThickness;
    out.textField.cursorThickness = tf["cursorThickness"] | out.textField.cursorThickness;
    out.textField.lineEndOffset = tf["lineEndOffset"] | out.textField.lineEndOffset;
  }

  // ─── Fonts ──────────────────────────────────────────────────────
  JsonObjectConst fonts = doc["fonts"];
  if (!fonts.isNull()) {
    parseFontSpecs(fonts, fontSpec);
  } else {
    memset(&fontSpec, 0, sizeof(fontSpec));
  }

  return true;
}
