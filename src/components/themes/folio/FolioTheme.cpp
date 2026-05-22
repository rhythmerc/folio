#include "FolioTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <string>

#include "CrossPointSettings.h"
#include "ThemeFontRegistry.h"
#include "fontIds.h"

namespace {
// Front-button hint geometry — must match BaseTheme::drawButtonHints so the
// on-screen labels stay aligned with the physical front buttons. The X4 and
// X3 use different X coordinates because their bezels are different widths.
constexpr int FOLIO_BUTTON_WIDTH = 106;
constexpr int FOLIO_BUTTON_HINT_RULE_WIDTH = 22;
constexpr int FOLIO_HEADER_BOTTOM_BORDER = 3;
constexpr int X4_BUTTON_POSITIONS[] = {25, 130, 245, 350};
constexpr int X3_BUTTON_POSITIONS[] = {38, 154, 268, 384};
}  // namespace

// ---- Font role mapping ------------------------------------------------------

int FolioTheme::getFontForRole(FontRole role) const { return resolveFontRole(role); }

int FolioTheme::resolveFontRole(FontRole role) {
  // Prefer an SD-installed face if the user has dropped one in
  // /.fonts/themes/folio/<role>.cpfont. The registry's lookup is a tiny
  // vector scan (≤ 5 entries per theme), so this is cheap to call per
  // drawText invocation.
  const int sdId = THEME_FONTS.getRoleFont("folio", role);
  if (sdId != 0) return sdId;

  // Embedded fallbacks. The smaller roles (Body/Caption/Accent) are sized
  // for prototype fidelity via SD overrides — NOTOSERIF_10 is the closest
  // embedded match but visually larger than the prototype's 11–16 px range.
  // Install /.fonts/themes/folio/<role>.cpfont to get the intended sizing.
  //
  // Title / Heading map to NOTOSERIF_14 (em ≈ 29 px), which matches the
  // prototype's 28 / 30 px CSS sizes far better than the older _16/_18
  // choice and stays within the embedded font set.
  switch (role) {
    case FontRole::Title:
      return NOTOSERIF_14_FONT_ID;
    case FontRole::Heading:
      return NOTOSERIF_14_FONT_ID;
    case FontRole::Body:
      return NOTOSERIF_12_FONT_ID;
    case FontRole::Caption:
      return NOTOSERIF_10_FONT_ID;
    case FontRole::Accent:
      return NOTOSERIF_10_FONT_ID;
  }
  return NOTOSERIF_12_FONT_ID;
}

// ---- Static helpers ---------------------------------------------------------

void FolioTheme::drawDiagonalHatch(const GfxRenderer& renderer, Rect rect, int spacingPx) {
  if (rect.width <= 0 || rect.height <= 0 || spacingPx <= 0) {
    return;
  }
  // Two passes of 45° lines. First pass starts each line at y=0 walking right;
  // second pass starts at x=0 walking down. Together they tile the rect with
  // a uniform diagonal stride of `spacingPx`.
  for (int x0 = 0; x0 < rect.width; x0 += spacingPx) {
    const int endX = std::min(x0 + rect.height - 1, rect.width - 1);
    const int endY = endX - x0;
    renderer.drawLine(rect.x + x0, rect.y, rect.x + endX, rect.y + endY);
  }
  for (int y0 = spacingPx; y0 < rect.height; y0 += spacingPx) {
    const int endY = std::min(y0 + rect.width - 1, rect.height - 1);
    const int endX = endY - y0;
    renderer.drawLine(rect.x, rect.y + y0, rect.x + endX, rect.y + endY);
  }
}

void FolioTheme::drawCornerBrackets(const GfxRenderer& renderer, Rect rect, int armPx, int strokePx) {
  const int rx = rect.x;
  const int ry = rect.y;
  const int rw = rect.width;
  const int rh = rect.height;

  // Top-left
  renderer.fillRect(rx, ry, armPx, strokePx);
  renderer.fillRect(rx, ry, strokePx, armPx);
  // Top-right
  renderer.fillRect(rx + rw - armPx, ry, armPx, strokePx);
  renderer.fillRect(rx + rw - strokePx, ry, strokePx, armPx);
  // Bottom-left
  renderer.fillRect(rx, ry + rh - strokePx, armPx, strokePx);
  renderer.fillRect(rx, ry + rh - armPx, strokePx, armPx);
  // Bottom-right
  renderer.fillRect(rx + rw - armPx, ry + rh - strokePx, armPx, strokePx);
  renderer.fillRect(rx + rw - strokePx, ry + rh - armPx, strokePx, armPx);
}

void FolioTheme::drawSelectionFrame(const GfxRenderer& renderer, Rect rect) {
  // 1-bit e-ink can't reproduce the prototype's 5% opacity diagonal hatch
  // legibly — even at sparse spacing it reads as rough shading. Outline +
  // corner brackets carry the selection on their own.
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  drawCornerBrackets(renderer, rect);
}

void FolioTheme::drawFolioButtonHint(const GfxRenderer& renderer, int x, int y, int buttonWidth, int buttonHeight,
                                     const char* label) {
  // 1px outline rectangle — no fill, so the paper background shows through.
  renderer.drawRect(x, y, buttonWidth, buttonHeight);

  // Use the Body role so an SD-installed /.fonts/themes/folio/body.cpfont
  // takes effect on the button labels too. Falls back to NOTOSERIF_12 when
  // no override is installed.
  const int bodyFont = resolveFontRole(FontRole::Body);

  // Anchor the rule at a fixed clearance from the button bottom (instead of
  // a flex offset from the text baseline) so it never falls under the X4
  // bezel, regardless of the font's vertical metrics.
  constexpr int ruleClearanceFromBottom = 12;
  const int lineHeight = renderer.getLineHeight(bodyFont);
  const int ruleY = y + buttonHeight - ruleClearanceFromBottom;

  // Center the label vertically in the area above the rule (with a small
  // gap so it isn't sitting on top of the rule itself).
  constexpr int textToRuleGap = 4;
  const int textBlockBottom = ruleY - textToRuleGap;
  const int textY = y + (textBlockBottom - y - lineHeight) / 2;

  const int textWidth = renderer.getTextWidth(bodyFont, label, EpdFontFamily::ITALIC);
  const int textX = x + (buttonWidth - textWidth) / 2;
  renderer.drawText(bodyFont, textX, textY, label, true, EpdFontFamily::ITALIC);

  const int ruleX = x + (buttonWidth - FOLIO_BUTTON_HINT_RULE_WIDTH) / 2;
  renderer.drawLine(ruleX, ruleY, ruleX + FOLIO_BUTTON_HINT_RULE_WIDTH - 1, ruleY);
}

// ---- Overrides --------------------------------------------------------------

void FolioTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // 3px inner bottom border lives WITHIN the 89px header height, per the
  // prototype spec. Drawing it as a filled rect (not a 1px line) so it has
  // proper visual weight on e-ink.
  renderer.fillRect(rect.x, rect.y + rect.height - FOLIO_HEADER_BOTTOM_BORDER, rect.width, FOLIO_HEADER_BOTTOM_BORDER);

  // Battery, top-right. Same blanking + redraw dance as BaseTheme to handle
  // partial-update artifacts.
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth,
                    FolioMetrics::values.batteryHeight + 10, false);
  const bool showBatteryPct =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - FolioMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, FolioMetrics::values.batteryWidth, FolioMetrics::values.batteryHeight},
                   showBatteryPct);

  const int contentPad = FolioMetrics::values.contentSidePadding;
  const int titleMaxWidth = batteryX - rect.x - contentPad * 2;
  const bool hasSubtitle = subtitle != nullptr && subtitle[0] != '\0';

  if (title != nullptr && title[0] != '\0') {
    // Shift the title up when a subtitle is present so the two-line stack
    // fits cleanly above the 3px inner bottom border. Otherwise sit slightly
    // lower for a more visually centered solo title.
    const int titleY = hasSubtitle ? 20 : 32;
    const std::string truncatedTitle =
        renderer.truncatedText(NOTOSERIF_18_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSERIF_18_FONT_ID, rect.x + contentPad, rect.y + titleY, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
  }

  if (hasSubtitle) {
    // Sits ~14px below the title baseline (after accounting for line height)
    // and clears the 3px inner border at y + 86.
    constexpr int subtitleY = 56;
    const std::string truncatedSubtitle = renderer.truncatedText(
        NOTOSERIF_10_FONT_ID, subtitle, rect.width - contentPad * 2, EpdFontFamily::ITALIC);
    renderer.drawText(NOTOSERIF_10_FONT_ID, rect.x + contentPad, rect.y + subtitleY, truncatedSubtitle.c_str(), true,
                      EpdFontFamily::ITALIC);
  }
}

void FolioTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                 const char* btn4) const {
  // drawButtonHints is one of the few render paths that forces portrait,
  // because the physical front buttons are always at the bottom of the
  // physical screen regardless of the active reading orientation.
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int buttonHeight = FolioMetrics::values.buttonHintsHeight;
  const int* positions = gpio.deviceIsX3() ? X3_BUTTON_POSITIONS : X4_BUTTON_POSITIONS;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; ++i) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    drawFolioButtonHint(renderer, positions[i], pageHeight - buttonHeight, FOLIO_BUTTON_WIDTH, buttonHeight, labels[i]);
  }

  renderer.setOrientation(origOrientation);
}

void FolioTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                          const std::function<std::string(int index)>& rowTitle,
                          const std::function<std::string(int index)>& rowSubtitle,
                          const std::function<UIIcon(int index)>& /*rowIcon*/,
                          const std::function<std::string(int index)>& rowValue, bool /*highlightValue*/,
                          const std::function<bool(int index)>& rowDimmed) const {
  const int rowHeight = (rowSubtitle != nullptr) ? FolioMetrics::values.listWithSubtitleRowHeight
                                                 : FolioMetrics::values.listRowHeight;
  const int pageItems = rect.height / rowHeight;
  if (pageItems <= 0) return;

  const int pageStartIndex = (selectedIndex < 0) ? 0 : (selectedIndex / pageItems) * pageItems;
  const int contentPad = FolioMetrics::values.contentSidePadding;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; ++i) {
    const int row = i - pageStartIndex;
    const int itemY = rect.y + row * rowHeight;
    const bool selected = (i == selectedIndex);

    if (selected) {
      drawSelectionFrame(renderer, Rect{rect.x + contentPad, itemY, rect.width - contentPad * 2, rowHeight});
    }

    const int textPad = contentPad + 8;
    int rowTextWidth = rect.width - textPad * 2;

    // Compute the value first so we can reserve its width before truncating
    // the title. Mirrors BaseTheme::drawList — without this, long settings
    // titles (e.g. "Clear Read Books from Recent List") run all the way to
    // the right edge and visually collide with their value text.
    constexpr int minValueGap = 10;
    constexpr int minTitleWidth = 40;
    std::string valueText;
    int valueDrawWidth = 0;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValW = std::max(0, rowTextWidth - minTitleWidth - minValueGap);
        valueText = renderer.truncatedText(NOTOSERIF_10_FONT_ID, valueText.c_str(), maxValW);
        valueDrawWidth = renderer.getTextWidth(NOTOSERIF_10_FONT_ID, valueText.c_str());
        rowTextWidth -= valueDrawWidth + minValueGap;
      }
    }

    // Title — 12pt serif for primary list content.
    const std::string itemName = rowTitle(i);
    const std::string truncatedTitle = renderer.truncatedText(NOTOSERIF_12_FONT_ID, itemName.c_str(), rowTextWidth);
    const int titleY = (rowSubtitle != nullptr) ? itemY + 8
                                                : itemY + (rowHeight - renderer.getLineHeight(NOTOSERIF_12_FONT_ID)) / 2;
    renderer.drawText(NOTOSERIF_12_FONT_ID, rect.x + textPad, titleY, truncatedTitle.c_str());

    // Subtitle — 10pt italic serif, true secondary visual weight.
    if (rowSubtitle != nullptr) {
      const std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        const std::string truncatedSub =
            renderer.truncatedText(NOTOSERIF_10_FONT_ID, subtitleText.c_str(), rowTextWidth, EpdFontFamily::ITALIC);
        renderer.drawText(NOTOSERIF_10_FONT_ID, rect.x + textPad, itemY + 30, truncatedSub.c_str(), true,
                          EpdFontFamily::ITALIC);
      }
    }

    if (!valueText.empty()) {
      // Right-align value baseline with the title's baseline. Both are now
      // different sizes, so use the title font's line height for the y math.
      const int valueLineHeight = renderer.getLineHeight(NOTOSERIF_10_FONT_ID);
      const int titleLineHeight = renderer.getLineHeight(NOTOSERIF_12_FONT_ID);
      const int valueY = titleY + (titleLineHeight - valueLineHeight);
      const int valueX = rect.x + rect.width - contentPad - valueDrawWidth - 4;
      renderer.drawText(NOTOSERIF_10_FONT_ID, valueX, valueY, valueText.c_str());
    }

    // Checkerboard dither over dimmed rows (mirrors BaseTheme behavior).
    if (rowDimmed != nullptr && rowDimmed(i) && !selected) {
      const int titleWidth = renderer.getTextWidth(NOTOSERIF_12_FONT_ID, truncatedTitle.c_str());
      const int lineH = renderer.getLineHeight(NOTOSERIF_12_FONT_ID);
      const int tx = rect.x + textPad;
      for (int py = titleY; py < titleY + lineH; ++py) {
        for (int px = tx; px < tx + titleWidth; ++px) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }
  }
}

void FolioTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                const std::function<std::string(int index)>& buttonLabel,
                                const std::function<UIIcon(int index)>& /*rowIcon*/) const {
  const int contentPad = FolioMetrics::values.contentSidePadding;
  const int tileWidth = rect.width - contentPad * 2;
  const int tileHeight = FolioMetrics::values.menuRowHeight;

  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = FolioMetrics::values.verticalSpacing + rect.y +
                      i * (FolioMetrics::values.menuRowHeight + FolioMetrics::values.menuSpacing);
    const int tileX = rect.x + contentPad;
    const bool selected = (i == selectedIndex);

    if (selected) {
      drawSelectionFrame(renderer, Rect{tileX, tileY, tileWidth, tileHeight});
    }

    const std::string labelStr = buttonLabel(i);
    const int textWidth = renderer.getTextWidth(NOTOSERIF_16_FONT_ID, labelStr.c_str(), EpdFontFamily::BOLD);
    const int textX = tileX + (tileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(NOTOSERIF_16_FONT_ID);
    const int textY = tileY + (tileHeight - lineHeight) / 2;
    renderer.drawText(NOTOSERIF_16_FONT_ID, textX, textY, labelStr.c_str(), true, EpdFontFamily::BOLD);
  }
}
