#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Folio — an editorial, typography-forward theme. Identifying visual cues:
//   * 89px header with a 3px inner bottom border
//   * Italic-serif button hints with a hairline rule beneath the label
//   * Selection treated with a 1px outline, four corner brackets, and a
//     45° diagonal hatch fill (instead of the usual solid-bar inversion)
//
// The drawing primitives that make up the selection treatment (hatch,
// corner brackets, and the composed "selection frame") are exposed as
// static methods so LibraryActivity can use the same visual language
// regardless of which theme is currently active.
namespace FolioMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 // Header matches the prototype spec (89px, with 3px inner border).
                                 .headerHeight = 89,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 18,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 56,
                                 .menuSpacing = 14,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 0,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class FolioTheme : public BaseTheme {
 public:
  // Header with 3px inner bottom border, bold-serif title, italic-serif subtitle.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;

  // Italic-serif label + 22px hairline rule, X4 firmware button positions.
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;

  // List rendered with the Folio selection frame (no solid-bar selection).
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int index)>& rowIcon = nullptr,
                const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = nullptr) const override;

  // Home menu uses the same selection frame as drawList.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  // --------------------------------------------------------------------
  // Folio drawing helpers — exposed as statics so LibraryActivity (and
  // anything else that wants the visual language) can call them without
  // depending on Folio being the active theme.

  // 45° hatch (lines from top-left to bottom-right). Lines clipped to rect.
  static void drawDiagonalHatch(const GfxRenderer& renderer, Rect rect, int spacingPx = 7);

  // Four corner brackets at the corners of rect (TL/TR/BL/BR).
  static void drawCornerBrackets(const GfxRenderer& renderer, Rect rect, int armPx = 8, int strokePx = 2);

  // Composed selection treatment: 1px outline + corner brackets + diagonal hatch.
  static void drawSelectionFrame(const GfxRenderer& renderer, Rect rect);

  // A single button hint — italic-serif label centered, 22px hairline rule below.
  // Exposed so LibraryActivity can render the same hint geometry without
  // duplicating the layout math.
  static void drawFolioButtonHint(const GfxRenderer& renderer, int x, int y, int buttonWidth, int buttonHeight,
                                  const char* label);
};
