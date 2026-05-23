#pragma once

#include <EpdFontFamily.h>

#include <cstdint>

#include "BaseTheme.h"  // for FontRole, UIIcon

// Declarative theme specification.
//
// A theme is a `ThemeData` value, not a class. `BaseTheme` holds a pointer to
// one and dispatches its draw methods on the enum fields below. Themes differ
// only in this data — no per-theme code paths anywhere else.
//
// Adding a new visual variant means either choosing existing enum values or
// (rarely) adding a new enum + a switch case in BaseTheme.

enum class HeaderStyle : uint8_t {
  // Title centered at top, subtitle right-aligned at a fixed screen-bottom y.
  // BaseTheme legacy.
  CenteredTitle,
  // Left-aligned bold title, right-aligned subtitle, full-width rule below.
  LeftAlignedWithRule,
  // Left-aligned bold title, optional italic subtitle stacked beneath, with
  // a `headerBottomBorderHeight`px filled border inside the header rect.
  LeftAlignedBordered,
  // Left-aligned bold title only; ignores subtitle. Drawn at a small
  // top inset matching the legacy RoundedRaff look.
  LeftAlignedPlain,
};

enum class ButtonHintsStyle : uint8_t {
  // 4 sharp-cornered rectangles at fixed X positions, label centered.
  Boxed,
  // 1-px outline only + italic-serif label + hairline rule beneath each label.
  Hairline,
  // Rounded white-filled buttons; labels appear inside, no-label buttons
  // shrink to a small placeholder.
  RoundedFilled,
  // Two rounded outline groups split L/R; each group holds two paired labels.
  PairedGroups,
};

enum class SideButtonHintsStyle : uint8_t {
  // 1-px sharp rectangles.
  Sharp,
  // Rounded rectangles, single-side corner radius (rounds away from screen
  // edge so the bezel-adjacent edge stays flush).
  Rounded,
};

enum class TabBarStyle : uint8_t {
  // Selected tab gets an underline (or solid bar if the bar itself is
  // focused). Sans-serif, no fill.
  Underline,
  // Selected tab gets a rounded fill (or dithered when bar is unfocused),
  // background of the entire bar dithered.
  DitheredRounded,
  // Each tab fills its slot with a rounded shape; selected tab inverts.
  SlotRounded,
};

enum class SelectionStyle : uint8_t {
  // Sharp-rect solid fill, text inverts. BaseTheme legacy.
  SolidFill,
  // Rounded LightGray fill over the selected row.
  RoundedFill,
  // Every row gets a rounded fill (Black if selected, White otherwise) — the
  // RoundedRaff look where rows are always visually pill-shaped.
  RoundedRowAlways,
  // Layered border: outer ink stroke + white gap + inner ink stroke, plus
  // diagonal corner brackets (TL + BR). Folio's signature treatment.
  LayeredFrame,
};

enum class ScrollIndicatorStyle : uint8_t {
  // Up/down chevron arrows at top/bottom-right of the list area.
  Arrows,
  // Solid thumb rect proportional to page size.
  Thumb,
  // Vertical track line + solid thumb rect.
  LineThumb,
};

enum class BatteryStyle : uint8_t {
  // Solid horizontal fill proportional to percentage.
  Solid,
  // 3 vertical segments lit at 10/40/70 % thresholds.
  Segmented,
};

enum class CoverStyle : uint8_t {
  // BaseTheme legacy — adaptive width book card with optional bookmark ribbon
  // and centered title/author block.
  Default,
  // Lyra layout — fixed cover on the left, title/author card on the right,
  // dithered selection wash around the cover.
  Card,
  // RoundedRaff layout — like Card but with rounded-corner mask on the
  // cover and rounded-rect chrome on the surrounding slate.
  CardRounded,
};

struct ThemeData {
  // Stable lowercase identifier — used to locate SD card fonts under
  // /.fonts/themes/<id>/<role>.cpfont. Must match a `THEME_FONTS` registry
  // theme name; declare as a string literal in BaseTheme.cpp so it lives
  // in flash.
  const char* id;

  // ─── Font role mapping ────────────────────────────────────────────
  // Embedded fallbacks. The theme registry overlays SD-installed faces
  // on top at lookup time. Compact variants (smaller faces for dense
  // screens like Library) fall through to their non-compact counterpart
  // when set to 0 — themes can ship just the base set.
  struct Fonts {
    int titleId;
    int headingId;
    int bodyId;
    int captionId;
    int accentId;
    int bodyIdCompact;
    int captionIdCompact;
    int accentIdCompact;
  } fonts;

  // ─── Header ─────────────────────────────────────────────────────
  struct Header {
    HeaderStyle style;
    EpdFontFamily::Style titleStyle;     // BOLD by default
    EpdFontFamily::Style subtitleStyle;  // REGULAR or ITALIC depending on theme
    int bottomBorderHeight;              // LeftAlignedBordered: 3 (Folio). 0 otherwise.
    int height;
  } header;

  // ─── Selection (style + corner radius + content text color) ──────
  struct Selection {
    SelectionStyle style;
    int cornerRadius;  // Used by RoundedFill / RoundedRowAlways.
    // True when the selection background is dark enough that the content
    // text needs to render in inverted (white) ink — typically the case
    // for SolidFill and RoundedRowAlways. Fill-light styles (RoundedFill
    // over LightGray) and border-only styles (LayeredFrame) keep regular
    // black text. Callers query GUI.getData()->selection.textInverted.
    bool textInverted;
  } selection;

  // ─── Cover (style + mask radius for rounded cover bitmaps) ───────
  struct Cover {
    CoverStyle style;
    int cornerRadius;  // Used by CardRounded for the cover bitmap mask.
  } cover;

  // ─── List + button-menu typography ───────────────────────────────
  struct List {
    EpdFontFamily::Style subtitleStyle;        // ITALIC for Folio, REGULAR elsewhere
    EpdFontFamily::Style buttonMenuLabelStyle; // BOLD for Folio/RoundedRaff
    int rowHeight;
    int rowHeightWithSubtitle;
    int menuRowHeight;
    int menuSpacing;
  } list;

  // ─── Stylistic dispatchers ──────────────────────────────────────
  ButtonHintsStyle buttonHintsStyle;
  SideButtonHintsStyle sideButtonHintsStyle;
  TabBarStyle tabBarStyle;
  ScrollIndicatorStyle scrollIndicatorStyle;
  BatteryStyle batteryStyle;

  // ─── Layout ─────────────────────────────────────────────────────
  struct Layout {
    int topPadding;
    int verticalSpacing;
    int contentSidePadding;
  } layout;

  struct Battery {
    int width;
    int height;
    int barHeight;
  } battery;

  struct ButtonHints {
    int height;
    int sideWidth;
  } buttonHints;

  struct TabBar {
    int spacing;
    int height;
  } tabBar;

  struct ScrollBar {
    int width;
    int rightOffset;
  } scrollBar;

  struct Home {
    int topPadding;
    int coverHeight;
    int coverTileHeight;
    int recentBooksCount;
    bool continueReadingInMenu;
    int menuTopOffset;
  } home;

  struct ProgressBar {
    int height;
    int marginTop;
  } progressBar;

  struct StatusBar {
    int horizontalMargin;
    int verticalMargin;
  } statusBar;

  struct Keyboard {
    int keyWidth;
    int keyHeight;
    int keySpacing;
    int bottomKeyHeight;
    int bottomKeySpacing;
    bool bottomAligned;
    bool centeredText;
    int verticalOffset;
    int textFieldWidthPercent;
    int widthPercent;
    int keyCornerRadius;
    bool fillUnselected;
    bool outlineAllUnselected;
    bool drawSpecialOutlineWhenUnselected;
    int secondaryLabelRightPadding;
    int secondaryLabelTopPadding;
    int minArrowHeadSize;
  } keyboard;

  struct Popup {
    float topOffsetRatio;
    int marginX;
    int marginY;
    int frameThickness;
    int cornerRadius;
    bool textBold;
    bool textInverted;
    int textBaselineOffsetY;
    struct Progress {
      int barHeight;
      bool drawOutline;
      bool clampPercent;
      bool fillInverted;
      bool outlineInverted;
    } progress;
  } popup;

  struct TextField {
    int horizontalPadding;
    int normalThickness;
    int cursorThickness;
    int lineEndOffset;
  } textField;

  // ─── Misc presentation flags ─────────────────────────────────────
  bool showsFileIcons;
};

// Built-in themes. Defined as static constexpr in BaseTheme.cpp so they live
// entirely in flash. Themes are picked by UITheme::setTheme().
namespace BuiltinThemes {
extern const ThemeData Classic;
extern const ThemeData Folio;
extern const ThemeData Lyra;
extern const ThemeData RoundedRaff;
}  // namespace BuiltinThemes
