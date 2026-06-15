#include "ButtonHints.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {

void drawHairlineHint(const GfxRenderer& renderer, int x, int y, int buttonWidth, int buttonHeight, const char* label,
                      int labelFontId) {
  // Outline only — no fill — so the paper background shows through.
  renderer.drawRect(x, y, buttonWidth, buttonHeight);

  // Anchor the rule at a fixed clearance from the button bottom so it never
  // falls under the X4 bezel regardless of the font's vertical metrics.
  constexpr int ruleClearanceFromBottom = 12;
  constexpr int ruleWidth = 22;
  constexpr int textToRuleGap = 4;
  const int lineHeight = renderer.getLineHeight(labelFontId);
  const int ruleY = y + buttonHeight - ruleClearanceFromBottom;
  const int textBlockBottom = ruleY - textToRuleGap;
  const int textY = y + (textBlockBottom - y - lineHeight) / 2;
  const int textWidth = renderer.getTextWidth(labelFontId, label, EpdFontFamily::ITALIC);
  const int textX = x + (buttonWidth - textWidth) / 2;
  renderer.drawText(labelFontId, textX, textY, label, true, EpdFontFamily::ITALIC);
  const int ruleX = x + (buttonWidth - ruleWidth) / 2;
  renderer.drawLine(ruleX, ruleY, ruleX + ruleWidth - 1, ruleY);
}

}  // namespace

bool ButtonHints::suppressed_ = false;

void ButtonHints::render(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                         const char* btn4) {
  if (suppressed_) return;

  const auto& theme = GUI;
  const auto& data = *theme.getData();

  const auto bodyCompactFont = GUI.getFontForRole(FontRole::BodyCompact);

  // Force portrait so the hints stay aligned with the physical front buttons
  // regardless of the active reading orientation.
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int buttonY = data.buttonHints.height;
  const int buttonHeight = data.buttonHints.height;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  switch (data.buttonHintsStyle) {
    case ButtonHintsStyle::Boxed: {
      constexpr int buttonWidth = 106;
      constexpr int textYOffset = 7;
      constexpr int x4Positions[] = {25, 130, 245, 350};
      constexpr int x3Positions[] = {38, 154, 268, 384};
      const int* positions = gpio.deviceIsX3() ? x3Positions : x4Positions;
      for (int i = 0; i < 4; i++) {
        if (labels[i] == nullptr || labels[i][0] == '\0') continue;
        const int x = positions[i];
        renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
        renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
        const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
        const int textX = x + (buttonWidth - 1 - textWidth) / 2;
        renderer.drawText(bodyCompactFont, textX, pageHeight - buttonY + textYOffset, labels[i]);
      }
      break;
    }
    case ButtonHintsStyle::Hairline: {
      constexpr int buttonWidth = 106;
      constexpr int x4Positions[] = {25, 130, 245, 350};
      constexpr int x3Positions[] = {38, 154, 268, 384};
      const int* positions = gpio.deviceIsX3() ? x3Positions : x4Positions;
      // Folio's button hints sit at the bottom of the screen aligned to the
      // physical button width — use the compact body face so the label has
      // breathing room inside the 106-px hint box. Falls through to the
      // regular body font when no compact face is installed.
      for (int i = 0; i < 4; ++i) {
        if (labels[i] == nullptr || labels[i][0] == '\0') continue;
        drawHairlineHint(renderer, positions[i], pageHeight - buttonHeight, buttonWidth, buttonHeight, labels[i],
                         bodyCompactFont);
      }
      break;
    }
    case ButtonHintsStyle::RoundedFilled: {
      constexpr int cornerRadius = 6;
      constexpr int buttonWidth = 80;
      constexpr int smallButtonHeight = 15;
      constexpr int textYOffset = 7;
      constexpr int x4Positions[] = {58, 146, 254, 342};
      constexpr int x3Positions[] = {65, 157, 291, 383};
      const int* positions = gpio.deviceIsX3() ? x3Positions : x4Positions;
      for (int i = 0; i < 4; i++) {
        const int x = positions[i];
        if (labels[i] != nullptr && labels[i][0] != '\0') {
          renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
          renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true,
                                   false, false, true);
          const int textWidth = renderer.getTextWidth(bodyCompactFont, labels[i]);
          const int textX = x + (buttonWidth - 1 - textWidth) / 2;
          renderer.drawText(bodyCompactFont, textX, pageHeight - buttonY + textYOffset, labels[i]);
        } else {
          renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                                   Color::White);
          renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius,
                                   true, true, false, false, true);
        }
      }
      break;
    }
    case ButtonHintsStyle::PairedGroups: {
      constexpr int sidePadding = 20;
      constexpr int groupGap = 10;
      constexpr int bottomMargin = 10;
      constexpr int groupCornerRadius = 15;
      constexpr int innerEdgePadding = 16;
      const int pageWidth = renderer.getScreenWidth();
      const int hintHeight = data.buttonHints.height - 10;
      const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
      const int hintY = pageHeight - hintHeight - bottomMargin;
      const int textY = hintY + (hintHeight - renderer.getLineHeight(bodyCompactFont)) / 2;

      const int leftGroupX = sidePadding;
      const int rightGroupX = leftGroupX + groupWidth + groupGap;
      const bool backDisabled = (labels[0] == nullptr || labels[0][0] == '\0');
      const std::string selectText = (labels[1] && labels[1][0] != '\0') ? labels[1] : "";
      const std::string upText = (labels[2] && labels[2][0] != '\0') ? labels[2] : "";
      const std::string downText = (labels[3] && labels[3][0] != '\0') ? labels[3] : "";

      // Clear behind the groups so other widgets can't bleed into the area.
      renderer.fillRect(leftGroupX, hintY, groupWidth, hintHeight, false);
      renderer.fillRect(rightGroupX, hintY, groupWidth, hintHeight, false);

      renderer.drawRoundedRect(leftGroupX, hintY, groupWidth, hintHeight, 2, groupCornerRadius, true);
      renderer.drawRoundedRect(rightGroupX, hintY, groupWidth, hintHeight, 2, groupCornerRadius, true);

      const int selectW = renderer.getTextWidth(bodyCompactFont, selectText.c_str(), EpdFontFamily::REGULAR);
      const int downW = renderer.getTextWidth(bodyCompactFont, downText.c_str(), EpdFontFamily::REGULAR);

      const int backX = leftGroupX + innerEdgePadding;
      const int selectX = leftGroupX + groupWidth - innerEdgePadding - selectW;
      const int upX = rightGroupX + innerEdgePadding;
      const int downX = rightGroupX + groupWidth - innerEdgePadding - downW;

      if (!backDisabled) renderer.drawText(bodyCompactFont, backX, textY, labels[0], true, EpdFontFamily::REGULAR);
      renderer.drawText(bodyCompactFont, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);
      renderer.drawText(bodyCompactFont, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
      renderer.drawText(bodyCompactFont, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);
      break;
    }
  }

  renderer.setOrientation(origOrientation);
}

void ButtonHints::renderSide(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                             const char* powerBtn) {
  if (suppressed_) return;

  const auto& data = *GUI.getData();
  const int screenWidth = renderer.getScreenWidth();
  const int buttonWidth = data.buttonHints.sideWidth;
  const bool rounded = data.sideButtonHintsStyle == SideButtonHintsStyle::Rounded;
  constexpr int roundedCornerRadius = 6;
  // Lyra uses a slightly taller side button + 0 margin; Sharp uses a 4-px
  // bezel margin and an 80-px height for the X3 layout.
  const int buttonHeight = rounded ? 78 : 80;
  const int buttonMargin = rounded ? 0 : 4;
  // Vertical gap between side buttons. Rounded leaves a 5-px breather; Sharp
  // shares its inner edge so the buttons read as a single bezel-aligned pair.
  const int sideButtonGap = rounded ? 5 : 0;
  const bool havePower = powerBtn != nullptr && powerBtn[0] != '\0';

  const auto bodyCompactFont = GUI.getFontForRole(FontRole::BodyCompact);

  if (gpio.deviceIsX3()) {
    constexpr int x3ButtonY = 155;
    const int leftX = buttonMargin;
    const int rightX = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      if (rounded) {
        renderer.drawRoundedRect(leftX, x3ButtonY, buttonWidth, buttonHeight, 1, roundedCornerRadius, false, true,
                                 false, true, true);
        const int textWidth = renderer.getTextWidth(bodyCompactFont, topBtn);
        renderer.drawTextRotated90CW(bodyCompactFont, leftX, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
      } else {
        renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
        const int textWidth = renderer.getTextWidth(bodyCompactFont, topBtn);
        const int textHeight = renderer.getTextHeight(bodyCompactFont);
        const int textX = leftX + (buttonWidth - textHeight) / 2;
        const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(bodyCompactFont, textX, textY, topBtn);
      }
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      if (rounded) {
        renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, roundedCornerRadius, true, false,
                                 true, false, true);
        const int textWidth = renderer.getTextWidth(bodyCompactFont, bottomBtn);
        renderer.drawTextRotated90CW(bodyCompactFont, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
      } else {
        renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
        const int textWidth = renderer.getTextWidth(bodyCompactFont, bottomBtn);
        const int textHeight = renderer.getTextHeight(bodyCompactFont);
        const int textX = rightX + (buttonWidth - textHeight) / 2;
        const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(bodyCompactFont, textX, textY, bottomBtn);
      }
    }
    return;
  }

  // X4 layout: stacked on the right side.
  const int x = screenWidth - buttonMargin - buttonWidth;
  const char* labels[] = {topBtn, bottomBtn};

  constexpr int topButtonY = 345;
  // Power-button slot. The Xteink X4's power button sits 15 mm above the
  // top side button's center — empirically (top→bottom) ≈ (power→top), so
  // we mirror the side-stack spacing: the power slot's bottom edge lands
  // `sideButtonGap` pixels above the top side button's top edge.
  const int powerY = topButtonY - buttonHeight - sideButtonGap;

  if (rounded) {
    if (havePower) {
      renderer.drawRoundedRect(x, powerY, buttonWidth, buttonHeight, 1, roundedCornerRadius, true, false, true, false,
                               true);
    }
    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topButtonY, buttonWidth, buttonHeight, 1, roundedCornerRadius, true, false, true,
                               false, true);
    }
    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, roundedCornerRadius,
                               true, false, true, false, true);
    }
    if (havePower) {
      const int textWidth = renderer.getTextWidth(bodyCompactFont, powerBtn);
      renderer.drawTextRotated90CW(bodyCompactFont, x, powerY + 5 + (buttonHeight + textWidth) / 2, powerBtn);
    }
    for (int i = 0; i < 2; i++) {
      if (labels[i] == nullptr || labels[i][0] == '\0') continue;
      const int y = topButtonY + i * buttonHeight + 5;
      const int textWidth = renderer.getTextWidth(bodyCompactFont, labels[i]);
      renderer.drawTextRotated90CW(bodyCompactFont, x, y + (buttonHeight + textWidth) / 2, labels[i]);
    }
    return;
  }

  // SideButtonHintsStyle::Sharp — BaseTheme legacy frame, three connected
  // lines so two adjacent buttons share their interior edge. The power slot
  // (when present) sits one button-height above the top side button and
  // gets its own standalone four-sided frame — it doesn't share an edge
  // with the page-turn pair below it.
  if (havePower) {
    renderer.drawRect(x, powerY, buttonWidth, buttonHeight);
    const int textWidth = renderer.getTextWidth(bodyCompactFont, powerBtn);
    const int textHeight = renderer.getTextHeight(bodyCompactFont);
    const int textX = x + (buttonWidth - textHeight) / 2;
    const int textY = powerY + (buttonHeight + textWidth) / 2;
    renderer.drawTextRotated90CW(bodyCompactFont, textX, textY, powerBtn);
  }
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
    renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
    renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
  }
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
  }
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
    renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);
    renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
  }
  for (int i = 0; i < 2; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const int y = topButtonY + i * buttonHeight;
    const int textWidth = renderer.getTextWidth(bodyCompactFont, labels[i]);
    const int textHeight = renderer.getTextHeight(bodyCompactFont);
    const int textX = x + (buttonWidth - textHeight) / 2;
    const int textY = y + (buttonHeight + textWidth) / 2;
    renderer.drawTextRotated90CW(bodyCompactFont, textX, textY, labels[i]);
  }
}
