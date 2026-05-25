#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {

// Shared paddings used by themes with the same-shape selection chrome.
constexpr int kSelectionHPad = 8;
constexpr int kMaxListValueWidth = 200;
constexpr int kSubtitleY = 738;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder: return Folder24Icon;
      case UIIcon::Text:   return Text24Icon;
      case UIIcon::Image:  return Image24Icon;
      case UIIcon::Book:   return Book24Icon;
      case UIIcon::File:   return File24Icon;
      default:             return nullptr;
    }
  }
  if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:   return FolderIcon;
      case UIIcon::Book:     return BookIcon;
      case UIIcon::Recent:   return RecentIcon;
      case UIIcon::Settings: return Settings2Icon;
      case UIIcon::Transfer: return TransferIcon;
      case UIIcon::Library:  return LibraryIcon;
      case UIIcon::Wifi:     return WifiIcon;
      case UIIcon::Hotspot:  return HotspotIcon;
      default:               return nullptr;
    }
  }
  return nullptr;
}

}  // namespace

// ============================================================================
// BaseTheme — constructors and font role lookup
// ============================================================================

BaseTheme::BaseTheme() : data(&BuiltinThemes::Folio) {}
BaseTheme::BaseTheme(const ThemeData* d) : data(d != nullptr ? d : &BuiltinThemes::Folio) {}

const char* BaseTheme::themeName() const { return data->id; }

int BaseTheme::resolveFontRole(const ThemeData& data, FontRole role) {
  switch (role) {
    case FontRole::Title:          return data.fonts.titleId;
    case FontRole::Heading:        return data.fonts.headingId;
    case FontRole::Body:           return data.fonts.bodyId;
    case FontRole::Caption:        return data.fonts.captionId;
    case FontRole::Accent:         return data.fonts.accentId;
    case FontRole::BodyCompact:    return data.fonts.bodyIdCompact != 0 ? data.fonts.bodyIdCompact : data.fonts.bodyId;
    case FontRole::CaptionCompact: return data.fonts.captionIdCompact != 0 ? data.fonts.captionIdCompact : data.fonts.captionId;
    case FontRole::AccentCompact:  return data.fonts.accentIdCompact != 0 ? data.fonts.accentIdCompact : data.fonts.accentId;
  }
  return data.fonts.bodyId;
}

int BaseTheme::getFontForRole(FontRole role) const { return resolveFontRole(*data, role); }

// ============================================================================
// Battery
// ============================================================================

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (data->batteryStyle == BatteryStyle::Segmented) {
    if (charging) {
      renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
      drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
    } else {
      if (percentage > 10) renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
      if (percentage > 40) renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
      if (percentage > 70) renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
    return;
  }

  // BatteryStyle::Solid
  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) return;
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) filledWidth = maxFillWidth;

  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }
  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);
  if (charging) drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }
  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }
  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

// ============================================================================
// Progress bar
// ============================================================================

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) return;
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);
  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

// ============================================================================
// Selection primitives (promoted from FolioTheme for shared use)
// ============================================================================

void BaseTheme::drawCornerBrackets(const GfxRenderer& renderer, Rect rect, int armPx, int strokePx) {
  const int rx = rect.x;
  const int ry = rect.y;
  const int rw = rect.width;
  const int rh = rect.height;
  // TL shifted +1 to overlap the white band of drawSelectionFrame's layered
  // border so the bracket renders on visible pixels. BR's natural placement
  // already overlaps the band at its bottom-right.
  renderer.fillRect(rx + 1, ry + 1, armPx, strokePx);
  renderer.fillRect(rx + 1, ry + 1, strokePx, armPx);
  renderer.fillRect(rx + rw - armPx, ry + rh - strokePx, armPx, strokePx);
  renderer.fillRect(rx + rw - strokePx, ry + rh - armPx, strokePx, armPx);
}

void BaseTheme::drawSelectionRect(const GfxRenderer& renderer, Rect rect, SelectionFill fill, SelectionBorder border,
                                  int cornerRadius) {
  // ---- Fill ----------------------------------------------------------------
  switch (fill) {
    case SelectionFill::None:
      break;
    case SelectionFill::Solid:
      if (cornerRadius > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cornerRadius, Color::Black);
      } else {
        renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      }
      break;
    case SelectionFill::LightGray:
      if (cornerRadius > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cornerRadius, Color::LightGray);
      } else {
        renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      }
      break;
  }

  // ---- Border --------------------------------------------------------------
  switch (border) {
    case SelectionBorder::None:
      return;
    case SelectionBorder::Single:
      if (cornerRadius > 0) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cornerRadius, true);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
      }
      return;
    case SelectionBorder::Double:
      // Outer 2 px ink → 1 px white gap → inner 2 px ink. Folio's signature.
      // No rounded variant — the layered look reads cleaner with sharp
      // corners on 1-bit e-ink.
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
      if (rect.width > 4 && rect.height > 4) {
        renderer.drawRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, 1, false);
      }
      if (rect.width > 6 && rect.height > 6) {
        renderer.drawRect(rect.x + 3, rect.y + 3, rect.width - 6, rect.height - 6, 2, true);
      }

      drawCornerBrackets(renderer, rect);
      return;
    case SelectionBorder::Brackets:
      drawCornerBrackets(renderer, rect);
      return;  // Brackets is standalone; don't compose with corner brackets twice.
  }
}

void BaseTheme::drawSelectionBackground(const GfxRenderer& renderer, Rect rect) const {
  // Fills only — drawn BEFORE the caller paints its content so covers /
  // text / icons end up on top of the wash. No-op for border-only styles.
  switch (data->selection.style) {
    case SelectionStyle::SolidFill:
      drawSelectionRect(renderer, rect, SelectionFill::Solid, SelectionBorder::None);
      break;
    case SelectionStyle::RoundedFill:
      drawSelectionRect(renderer, rect, SelectionFill::LightGray, SelectionBorder::None,
                        data->selection.cornerRadius);
      break;
    case SelectionStyle::RoundedRowAlways:
      // For tile-grid contexts (Library), RoundedRaff gets the same rounded
      // inversion it uses for list rows.
      drawSelectionRect(renderer, rect, SelectionFill::Solid, SelectionBorder::None,
                        data->selection.cornerRadius);
      break;
    case SelectionStyle::LayeredFrame:
      // No background — the layered border + brackets are foreground-only.
      break;
  }
}

void BaseTheme::drawSelectionForeground(const GfxRenderer& renderer, Rect rect) const {
  // Borders / brackets — drawn AFTER content so they sit on top. No-op for
  // fill-only styles.
  switch (data->selection.style) {
    case SelectionStyle::SolidFill:
    case SelectionStyle::RoundedFill:
    case SelectionStyle::RoundedRowAlways:
      // Fill-only — nothing to overlay.
      break;
    case SelectionStyle::LayeredFrame:
      drawSelectionRect(renderer, rect, SelectionFill::None, SelectionBorder::Double);
      break;
  }
}

void BaseTheme::drawSelectionFrame(const GfxRenderer& renderer, Rect rect) {
  drawSelectionRect(renderer, rect, SelectionFill::None, SelectionBorder::Double);
}

// ============================================================================
// Popup menu (cascading contextual menu, e.g. Library Sort/Files/Settings)
// ============================================================================

void BaseTheme::drawPopupMenu(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                              const std::function<std::string(int index)>& rowLabel,
                              const std::function<PopupMenuGlyph(int index)>& rowGlyph, int mutedIndex) const {
  if (itemCount <= 0) return;
  const auto& pm = data->popupMenu;

  // ---- Drop shadow ---------------------------------------------------------
  if (pm.shadowOffsetX != 0 || pm.shadowOffsetY != 0) {
    if (pm.cornerRadius > 0) {
      renderer.fillRoundedRect(rect.x + pm.shadowOffsetX, rect.y + pm.shadowOffsetY, rect.width, rect.height,
                               pm.cornerRadius, Color::Black);
    } else {
      renderer.fillRect(rect.x + pm.shadowOffsetX, rect.y + pm.shadowOffsetY, rect.width, rect.height, true);
    }
  }

  // ---- Panel fill (white) --------------------------------------------------
  if (pm.cornerRadius > 0) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, pm.cornerRadius, Color::White);
  } else {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  }

  // ---- Panel border --------------------------------------------------------
  if (pm.borderThickness > 0) {
    if (pm.cornerRadius > 0) {
      renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, pm.borderThickness, pm.cornerRadius, true);
    } else {
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, pm.borderThickness, true);
    }
  }

  // ---- Rows ----------------------------------------------------------------
  const int labelFontId = getFontForRole(pm.fontRole);
  const int annotFontId = getFontForRole(pm.annotationFontRole);
  const int labelLineHeight = renderer.getLineHeight(labelFontId);
  const int innerX = rect.x + pm.borderThickness;
  const int innerW = rect.width - 2 * pm.borderThickness;

  for (int i = 0; i < itemCount; ++i) {
    const Rect rowRect{innerX, rect.y + pm.borderThickness + i * pm.rowHeight, innerW, pm.rowHeight};

    const bool isSelected = (i == selectedIndex);
    const bool isMuted = (i == mutedIndex) && pm.subPanelMutedFill;

    // Background pass.
    if (isMuted && !isSelected) {
      renderer.fillRectDither(rowRect.x, rowRect.y, rowRect.width, rowRect.height, Color::LightGray);
    }
    if (isSelected) {
      switch (pm.selectionStyle) {
        case PopupMenuSelectionStyle::SolidFill:
          if (pm.selectionCornerRadius > 0) {
            renderer.fillRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, pm.selectionCornerRadius,
                                     Color::Black);
          } else {
            renderer.fillRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, true);
          }
          break;
        case PopupMenuSelectionStyle::RoundedFill:
          renderer.fillRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   pm.selectionCornerRadius > 0 ? pm.selectionCornerRadius : 4, Color::Black);
          break;
        case PopupMenuSelectionStyle::BorderOnly:
          renderer.drawRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, true);
          break;
      }
    }

    const bool textBlack = !(isSelected && pm.selectionTextInverted);

    // Label — vertically centered within the row.
    std::string label = rowLabel ? rowLabel(i) : std::string{};
    if (!label.empty()) {
      const int labelY = rowRect.y + (rowRect.height - labelLineHeight) / 2;
      renderer.drawText(labelFontId, rowRect.x + pm.paddingX, labelY, label.c_str(), textBlack);
    }

    // Optional right-aligned glyph. Drawn as a filled triangle so it doesn't
    // depend on the font having ↑/↓/▶ in its glyph set.
    if (rowGlyph) {
      const PopupMenuGlyph glyph = rowGlyph(i);
      if (glyph != PopupMenuGlyph::None) {
        // Glyph metrics — sized off the row height so it scales with theme.
        // Body of the glyph hugs the trailing edge with a paddingX gutter.
        const int glyphSize = std::max(6, rowRect.height / 3);
        const int gx = rowRect.x + rowRect.width - pm.paddingX - glyphSize;
        const int gy = rowRect.y + (rowRect.height - glyphSize) / 2;
        switch (glyph) {
          case PopupMenuGlyph::ArrowUp: {
            const int xs[3] = {gx + glyphSize / 2, gx, gx + glyphSize};
            const int ys[3] = {gy, gy + glyphSize, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case PopupMenuGlyph::ArrowDown: {
            const int xs[3] = {gx, gx + glyphSize, gx + glyphSize / 2};
            const int ys[3] = {gy, gy, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case PopupMenuGlyph::ChevronRight: {
            // Slightly narrower than the up/down arrows to read as "expand"
            // rather than "direction".
            const int w = (glyphSize * 2) / 3;
            const int cx = rowRect.x + rowRect.width - pm.paddingX - w;
            const int xs[3] = {cx, cx + w, cx};
            const int ys[3] = {gy, gy + glyphSize / 2, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case PopupMenuGlyph::None:
            break;
        }
      }
    }
    (void)annotFontId;  // annotation font reserved for future text glyphs
  }
}

// ============================================================================
// Header / SubHeader
// ============================================================================

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  const auto& m = *data;
  const int titleFont = getFontForRole(FontRole::Title);
  const int captionFont = getFontForRole(FontRole::Caption);
  const bool hasTitle = title != nullptr && title[0] != '\0';
  const bool hasSubtitle = subtitle != nullptr && subtitle[0] != '\0';

  switch (data->header.style) {
    case HeaderStyle::CenteredTitle: {
      constexpr int maxBatteryWidth = 80;
      renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth, m.battery.height + 10, false);
      const bool showPct =
          SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
      const int batteryX = rect.x + rect.width - 12 - m.battery.width;
      drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, m.battery.width, m.battery.height}, showPct);

      if (hasTitle) {
        const int padding = rect.width - batteryX + m.battery.width;
        auto truncated =
            renderer.truncatedText(titleFont, title, rect.width - padding * 2 - m.layout.contentSidePadding * 2,
                                   data->header.titleStyle);
        renderer.drawCenteredText(titleFont, rect.y + 5, truncated.c_str(), true, data->header.titleStyle);
      }
      if (hasSubtitle) {
        auto truncated = renderer.truncatedText(captionFont, subtitle, rect.width - m.layout.contentSidePadding * 2,
                                                data->header.subtitleStyle);
        const int subtitleW = renderer.getTextWidth(captionFont, truncated.c_str());
        renderer.drawText(captionFont, rect.x + rect.width - m.layout.contentSidePadding - subtitleW, kSubtitleY,
                          truncated.c_str(), true);
      }
      break;
    }
    case HeaderStyle::LeftAlignedWithRule: {
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
      const bool showPct =
          SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
      const int batteryX = rect.x + rect.width - 12 - m.battery.width;
      drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, m.battery.width, m.battery.height}, showPct);

      int titleW = hasTitle ? renderer.getTextWidth(titleFont, title, data->header.titleStyle) : 0;
      int subtitleW =
          hasSubtitle ? renderer.getTextWidth(captionFont, subtitle, data->header.subtitleStyle) : 0;
      const int available = rect.width - m.layout.contentSidePadding * 3;
      if (titleW + subtitleW > available) {
        if (titleW > available / 2 && subtitleW > available / 2) {
          titleW = available / 2;
          subtitleW = available / 2;
        } else if (titleW > subtitleW) {
          titleW = available - subtitleW;
        } else {
          subtitleW = available - titleW;
        }
      }
      if (hasTitle) {
        auto truncated = renderer.truncatedText(titleFont, title, titleW, data->header.titleStyle);
        renderer.drawText(titleFont, rect.x + m.layout.contentSidePadding, rect.y + m.battery.barHeight + 3,
                          truncated.c_str(), true, data->header.titleStyle);
        // Full-width rule at the bottom of the header.
        renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3,
                          true);
      }
      if (hasSubtitle) {
        auto truncated = renderer.truncatedText(captionFont, subtitle, subtitleW, data->header.subtitleStyle);
        const int sw = renderer.getTextWidth(captionFont, truncated.c_str());
        renderer.drawText(captionFont, rect.x + rect.width - m.layout.contentSidePadding - sw, rect.y + 50,
                          truncated.c_str(), true);
      }
      break;
    }
    case HeaderStyle::LeftAlignedBordered: {
      const int border = data->header.bottomBorderHeight;
      if (border > 0) {
        renderer.fillRect(rect.x, rect.y + rect.height - border, rect.width, border);
      }

      constexpr int maxBatteryWidth = 80;
      renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth, m.battery.height + 10, false);
      const bool showPct =
          SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
      const int batteryX = rect.x + rect.width - 12 - m.battery.width;
      drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, m.battery.width, m.battery.height}, showPct);

      const int contentPad = m.layout.contentSidePadding;
      const int titleMaxWidth = batteryX - rect.x - contentPad * 2;
      // Header title — go through the role registry so an SD-installed
      // title.cpfont overrides the embedded fallback, and the visual size
      // stays consistent with any other activity that resolves Title (e.g.
      // LibraryActivity's hand-rolled renderHeader). Previously this
      // shortcut to NOTOSERIF_18 to give the header more weight, but that
      // (a) ignored SD overrides, and (b) drifted Settings's title size
      // away from Library's.
      const int headerTitleFont = getFontForRole(FontRole::Title);
      if (hasTitle) {
        // Shift up when a subtitle is present so both lines fit cleanly above
        // the bottom border.
        const int titleY = hasSubtitle ? 20 : 32;
        auto truncated =
            renderer.truncatedText(headerTitleFont, title, titleMaxWidth, data->header.titleStyle);
        renderer.drawText(headerTitleFont, rect.x + contentPad, rect.y + titleY, truncated.c_str(), true,
                          data->header.titleStyle);
      }
      if (hasSubtitle) {
        constexpr int subtitleY = 56;
        auto truncated =
            renderer.truncatedText(captionFont, subtitle, rect.width - contentPad * 2, data->header.subtitleStyle);
        renderer.drawText(captionFont, rect.x + contentPad, rect.y + subtitleY, truncated.c_str(), true,
                          data->header.subtitleStyle);
      }
      break;
    }
    case HeaderStyle::LeftAlignedPlain: {
      if (!hasTitle) return;
      const int sidePadding = m.layout.contentSidePadding;
      const int titleX = rect.x + sidePadding;
      const int titleY = rect.y + 14;
      const bool showPct =
          SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
      const int batteryIconX = rect.x + rect.width - sidePadding - m.battery.width;

      int batteryGroupLeftX = batteryIconX;
      if (showPct) {
        const int maxTextWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
        batteryGroupLeftX -= maxTextWidth + batteryPercentSpacing;
        const int clearW = maxTextWidth + batteryPercentSpacing + m.battery.width;
        const int clearH = std::max(renderer.getTextHeight(SMALL_FONT_ID), m.battery.height + 8);
        renderer.fillRect(batteryIconX - maxTextWidth - batteryPercentSpacing, rect.y + 14, clearW, clearH, false);
      }
      const int maxTitleWidth = std::max(0, batteryGroupLeftX - 20 - titleX);
      auto truncated = renderer.truncatedText(titleFont, title, maxTitleWidth, data->header.titleStyle);
      renderer.drawText(titleFont, titleX, titleY, truncated.c_str(), true, data->header.titleStyle);
      drawBatteryRight(renderer, Rect{batteryIconX, rect.y + 14, m.battery.width, m.battery.height}, showPct);
      break;
    }
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const auto& m = *data;
  const int titleFont = getFontForRole(FontRole::Title);
  int currentX = rect.x + m.layout.contentSidePadding;
  int rightSpace = m.layout.contentSidePadding;
  if (rightLabel) {
    auto truncated = renderer.truncatedText(SMALL_FONT_ID, rightLabel, kMaxListValueWidth, EpdFontFamily::REGULAR);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - m.layout.contentSidePadding - w, rect.y + 7, truncated.c_str());
    rightSpace += w + 10;
  }
  auto truncatedLabel = renderer.truncatedText(titleFont, label, rect.width - m.layout.contentSidePadding - rightSpace,
                                               EpdFontFamily::REGULAR);
  const int labelY = data->header.style == HeaderStyle::LeftAlignedWithRule ? rect.y + 6 : rect.y;
  renderer.drawText(titleFont, currentX, labelY, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
  if (data->header.style == HeaderStyle::LeftAlignedWithRule) {
    renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
  }
}

// ============================================================================
// Tab bar
// ============================================================================

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  const auto& m = *data;

  int tabFont = getFontForRole(data->tabBar.fontRole);

  switch (data->tabBar.style) {
    case TabBarStyle::Underline: {
      constexpr int underlineHeight = 2;
      constexpr int underlineGap = 4;
      const int lineHeight = renderer.getLineHeight(tabFont);
      int currentX = rect.x + m.layout.contentSidePadding;
      for (const auto& tab : tabs) {
        const int textW =
            renderer.getTextWidth(tabFont, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        if (tab.selected) {
          if (selected) {
            renderer.fillRect(currentX - 3, rect.y, textW + 6, lineHeight + underlineGap);
          } else {
            renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textW, underlineHeight);
          }
        }
        renderer.drawText(tabFont, currentX, rect.y, tab.label, !(tab.selected && selected),
                          tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        currentX += textW + m.tabBar.spacing;
      }
      break;
    }
    case TabBarStyle::DitheredRounded: {
      const int bodyFont = getFontForRole(FontRole::Body);
      int currentX = rect.x + m.layout.contentSidePadding;
      if (selected) renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      for (const auto& tab : tabs) {
        const int textW = renderer.getTextWidth(bodyFont, tab.label, EpdFontFamily::REGULAR);
        if (tab.selected) {
          if (selected) {
            renderer.fillRoundedRect(currentX, rect.y + 1, textW + 2 * kSelectionHPad, rect.height - 4,
                                     data->selection.cornerRadius, Color::Black);
          } else {
            renderer.fillRectDither(currentX, rect.y, textW + 2 * kSelectionHPad, rect.height - 3, Color::LightGray);
            renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textW + 2 * kSelectionHPad,
                              rect.y + rect.height - 3, 2, true);
          }
        }
        renderer.drawText(bodyFont, currentX + kSelectionHPad, rect.y + 6, tab.label, !(tab.selected && selected),
                          EpdFontFamily::REGULAR);
        currentX += textW + m.tabBar.spacing + 2 * kSelectionHPad;
      }
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
      break;
    }
    case TabBarStyle::SlotRounded: {
      if (tabs.empty()) return;
      const int slotWidth = rect.width / static_cast<int>(tabs.size());
      const int tabY = rect.y + 4;
      const int tabHeight = rect.height - 12;
      for (size_t i = 0; i < tabs.size(); ++i) {
        const int slotX = rect.x + static_cast<int>(i) * slotWidth;
        const int tabX = slotX + 4;
        const int tabWidth = slotWidth - 8;
        const auto& tab = tabs[i];
        if (tab.selected) {
          renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, 18, selected ? Color::Black : Color::DarkGray);
        }
        const int textW = renderer.getTextWidth(tabFont, tab.label, EpdFontFamily::BOLD);
        const int textX = slotX + (slotWidth - textW) / 2;
        const int textY = tabY + (tabHeight - renderer.getLineHeight(tabFont)) / 2;
        renderer.drawText(tabFont, textX, textY, tab.label, !tab.selected, EpdFontFamily::BOLD);
      }
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
      break;
    }
  }
}

// ============================================================================
// List
// ============================================================================

namespace {

// Draw the configured scroll indicator on the right edge of `rect`.
void drawScrollIndicator(const ThemeData& data, const GfxRenderer& renderer, Rect rect, int itemCount,
                         int pageItems, int pageStartIndex) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) return;

  switch (data.scrollIndicatorStyle) {
    case ScrollIndicatorStyle::Arrows: {
      constexpr int indicatorWidth = 20;
      constexpr int arrowSize = 6;
      constexpr int margin = 15;
      const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
      const int indicatorTop = rect.y;
      const int indicatorBottom = rect.y + rect.height - arrowSize;
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + i * 2;
        const int startX = centerX - i;
        renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
      }
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
        const int startX = centerX - (arrowSize - 1 - i);
        renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                          indicatorBottom - arrowSize + 1 + i);
      }
      break;
    }
    case ScrollIndicatorStyle::Thumb: {
      const int barW = data.scrollBar.width;
      const int barX = rect.x + rect.width - data.scrollBar.rightOffset - barW;
      const int barY = rect.y;
      const int barH = rect.height;
      const int thumbH = std::max(10, (barH * pageItems) / itemCount);
      const int maxStart = std::max(1, itemCount - pageItems);
      const int maxTravel = std::max(1, barH - thumbH);
      const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
      const int thumbY = barY + (clampedStart * maxTravel) / maxStart;
      renderer.fillRect(barX, thumbY, barW, thumbH);
      break;
    }
    case ScrollIndicatorStyle::LineThumb: {
      const int barW = data.scrollBar.width;
      const int barX = rect.x + rect.width - data.scrollBar.rightOffset;
      const int barY = rect.y;
      const int barH = rect.height;
      const int totalPages = (itemCount + pageItems - 1) / pageItems;
      const int thumbH = (barH * pageItems) / itemCount;
      const int currentPage = pageStartIndex / pageItems;
      const int thumbY = barY + ((barH - thumbH) * currentPage) / std::max(1, totalPages - 1);
      renderer.drawLine(barX, barY, barX, barY + barH, true);
      renderer.fillRect(barX - barW, thumbY, barW, thumbH, true);
      break;
    }
  }
}

}  // namespace

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  const auto& m = *data;
  const int bodyFont = getFontForRole(FontRole::Body);
  const int captionFont = getFontForRole(FontRole::Caption);
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);

  // Per-row layout — RoundedRowAlways uses an extra-tall subtitle row.
  int rowHeight;
  int rowStep;
  if (data->selection.style == SelectionStyle::RoundedRowAlways && hasSubtitle) {
    constexpr int subtitleTopPadding = 10;
    constexpr int subtitleBottomPadding = 10;
    constexpr int subtitleInterLineGap = 4;
    const int subtitleLineHeight = renderer.getLineHeight(captionFont);
    const int titleLineHeight = renderer.getLineHeight(bodyFont);
    rowHeight = subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
    rowStep = rowHeight + 6;
  } else if (data->selection.style == SelectionStyle::RoundedRowAlways) {
    rowHeight = m.list.rowHeight;
    rowStep = rowHeight + 6;
  } else {
    rowHeight = hasSubtitle ? m.list.rowHeightWithSubtitle : m.list.rowHeight;
    rowStep = rowHeight;
  }
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = (selectedIndex < 0) ? 0 : (selectedIndex / pageItems) * pageItems;

  drawScrollIndicator(*data, renderer, rect, itemCount, pageItems, pageStartIndex);

  // Style-specific scroll reservation.
  int contentWidth = rect.width;
  if (data->scrollIndicatorStyle == ScrollIndicatorStyle::LineThumb && itemCount > pageItems) {
    contentWidth -= (m.scrollBar.width + m.scrollBar.rightOffset);
  } else if (data->scrollIndicatorStyle == ScrollIndicatorStyle::Arrows) {
    contentWidth -= 5;
  }

  const int contentPad = m.layout.contentSidePadding;
  const int iconSize = hasSubtitle ? 32 : 24;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; ++i) {
    const int row = i - pageStartIndex;
    const int itemY = rect.y + row * rowStep;
    const bool selected = (i == selectedIndex);

    // ----- Selection background -----------------------------------------
    switch (data->selection.style) {
      case SelectionStyle::SolidFill:
        if (selected) {
          renderer.fillRect(rect.x, itemY - 2, rect.width, rowHeight);
        }
        break;
      case SelectionStyle::RoundedFill:
        if (selected) {
          renderer.fillRoundedRect(rect.x + contentPad, itemY, contentWidth - contentPad * 2, rowHeight,
                                   data->selection.cornerRadius, Color::LightGray);
        }
        break;
      case SelectionStyle::RoundedRowAlways: {
        const int rowX = rect.x + contentPad;
        const int rowW = rect.width - contentPad * 2;
        renderer.fillRoundedRect(rowX, itemY, rowW, rowHeight, data->selection.cornerRadius,
                                 selected ? Color::Black : Color::White);
        break;
      }
      case SelectionStyle::LayeredFrame:
        if (selected) {
          drawSelectionFrame(renderer,
                             Rect{rect.x + contentPad, itemY, rect.width - contentPad * 2, rowHeight});
        }
        break;
    }

    // ----- Inset for the row content ------------------------------------
    int textX = rect.x + contentPad;
    int textWidth = contentWidth - contentPad * 2;
    if (data->selection.style == SelectionStyle::RoundedFill ||
        data->selection.style == SelectionStyle::RoundedRowAlways) {
      textX += kSelectionHPad;
      textWidth -= kSelectionHPad * 2;
    }
    if (data->selection.style == SelectionStyle::LayeredFrame) {
      textX += 8;
      textWidth -= 16;
    }
    if (rowIcon != nullptr && data->showsFileIcons) {
      textX += iconSize + kSelectionHPad;
      textWidth -= iconSize + kSelectionHPad;
    }

    // ----- Value ---------------------------------------------------------
    int valueWidth = 0;
    std::string valueText;
    constexpr int minValueGap = 10;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValW = std::max(0, textWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(bodyFont, valueText.c_str(), maxValW);
        valueWidth = renderer.getTextWidth(bodyFont, valueText.c_str()) + minValueGap;
        textWidth -= valueWidth;
      }
    }

    // ----- Title ---------------------------------------------------------
    const std::string itemName = rowTitle(i);
    const std::string truncated = renderer.truncatedText(bodyFont, itemName.c_str(), textWidth,
                                                         data->list.buttonMenuLabelStyle);
    // Themes declare `textInverted` when their selection background is
    // dark enough that content text needs to render in white ink.
    const bool invertText = selected && data->selection.textInverted;
    int titleY;
    if (hasSubtitle) {
      titleY = (data->selection.style == SelectionStyle::RoundedRowAlways) ? itemY + 10 : itemY + 8;
    } else {
      titleY = itemY + (rowHeight - renderer.getLineHeight(bodyFont)) / 2;
    }
    renderer.drawText(bodyFont, textX, titleY, truncated.c_str(), !invertText,
                      data->list.buttonMenuLabelStyle == EpdFontFamily::BOLD ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    // ----- Dimmed checkerboard ------------------------------------------
    if (rowDimmed && rowDimmed(i) && !selected) {
      const int titleW = renderer.getTextWidth(bodyFont, truncated.c_str());
      const int lineH = renderer.getLineHeight(bodyFont);
      for (int py = titleY; py < titleY + lineH; ++py) {
        for (int px = textX; px < textX + titleW; ++px) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }

    // ----- Icon ----------------------------------------------------------
    if (rowIcon != nullptr && data->showsFileIcons) {
      const uint8_t* iconBitmap = iconForName(rowIcon(i), iconSize);
      if (iconBitmap != nullptr) {
        const int iconY = hasSubtitle ? itemY + 16 : itemY + 10;
        renderer.drawIcon(iconBitmap, rect.x + contentPad + kSelectionHPad, iconY, iconSize, iconSize);
      }
    }

    // ----- Subtitle ------------------------------------------------------
    if (hasSubtitle) {
      const std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        const std::string truncatedSub =
            renderer.truncatedText(captionFont, subtitleText.c_str(), textWidth, data->list.subtitleStyle);
        const int subtitleY = (data->selection.style == SelectionStyle::RoundedRowAlways)
                                  ? titleY + renderer.getLineHeight(bodyFont) + 4
                                  : itemY + 30;
        renderer.drawText(captionFont, textX, subtitleY, truncatedSub.c_str(), !invertText, data->list.subtitleStyle);
      }
    }

    // ----- Value text ----------------------------------------------------
    if (!valueText.empty()) {
      const int valueX = rect.x + contentWidth - contentPad - valueWidth + minValueGap;
      int valueY = titleY;
      if (hasSubtitle) valueY = itemY + 10;
      const bool valueInverted = selected && highlightValue;
      if (valueInverted) {
        renderer.fillRoundedRect(valueX - kSelectionHPad / 2, itemY, valueWidth, rowHeight,
                                 data->selection.cornerRadius, Color::Black);
      }
      renderer.drawText(bodyFont, valueX, valueY, valueText.c_str(), !invertText && !valueInverted);
    }
  }
}

// ============================================================================
// Button menu
// ============================================================================

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& m = *data;
  const int labelFont = getFontForRole(FontRole::Heading);
  const int rowH = m.list.menuRowHeight;
  const int rowStep = rowH + m.list.menuSpacing;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelected = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelected / pageItems) * pageItems;
  const int sidePad = m.layout.contentSidePadding;

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const std::string labelStr = buttonLabel(i);
    const int rowY = m.layout.verticalSpacing + rect.y + (i - pageStartIndex) * rowStep;
    const int rowX = rect.x + sidePad;
    const int rowW = rect.width - sidePad * 2;
    const bool selected = (i == selectedIndex);

    // ----- Background ----------------------------------------------------
    switch (data->selection.style) {
      case SelectionStyle::SolidFill:
        if (selected)
          renderer.fillRect(rowX, rowY, rowW, rowH);
        else
          renderer.drawRect(rowX, rowY, rowW, rowH);
        break;
      case SelectionStyle::RoundedFill:
        if (selected) {
          renderer.fillRoundedRect(rowX, rowY, rowW, rowH, data->selection.cornerRadius, Color::LightGray);
        }
        break;
      case SelectionStyle::RoundedRowAlways: {
        // The legacy RoundedRaff width auto-fits to label width — kept here
        // so menus that share the row chrome read consistently.
        const int textW = renderer.getTextWidth(labelFont, labelStr.c_str(), data->list.buttonMenuLabelStyle);
        constexpr int padX = 40;
        const int autoW = std::min(rowW, textW + padX);
        renderer.fillRoundedRect(rowX, rowY, autoW, rowH, data->selection.cornerRadius,
                                 selected ? Color::Black : Color::White);
        break;
      }
      case SelectionStyle::LayeredFrame:
        if (selected) drawSelectionFrame(renderer, Rect{rowX, rowY, rowW, rowH});
        break;
    }

    // ----- Label + optional icon ----------------------------------------
    const int textWidth = renderer.getTextWidth(labelFont, labelStr.c_str(), data->list.buttonMenuLabelStyle);
    const int lineHeight = renderer.getLineHeight(labelFont);
    int textX;
    if (data->showsFileIcons && rowIcon != nullptr) {
      constexpr int mainMenuIconSize = 32;
      textX = rowX + 16;
      const uint8_t* iconBitmap = iconForName(rowIcon(i), mainMenuIconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, rowY + (rowH - mainMenuIconSize) / 2 + 3, mainMenuIconSize,
                          mainMenuIconSize);
        textX += mainMenuIconSize + kSelectionHPad + 2;
      }
    } else if (data->selection.style == SelectionStyle::RoundedRowAlways) {
      // Left-anchored label with inset.
      textX = rowX + 20;
    } else {
      // Centered label.
      textX = rect.x + (rect.width - textWidth) / 2;
    }
    const int textY = rowY + (rowH - lineHeight) / 2;
    // Themes declare `textInverted` when their selection background is
    // dark enough that content text needs to render in white ink.
    const bool invertText = selected && data->selection.textInverted;
    renderer.drawText(labelFont, textX, textY, labelStr.c_str(), !invertText, data->list.buttonMenuLabelStyle);
  }

  if (data->scrollIndicatorStyle == ScrollIndicatorStyle::Thumb) {
    drawScrollIndicator(*data, renderer, rect, buttonCount, pageItems, pageStartIndex);
  }
}

// ============================================================================
// Popup
// ============================================================================

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& m = *data;
  const int marginX = m.popup.marginX;
  const int marginY = m.popup.marginY;
  const int frameThickness = m.popup.frameThickness;
  const EpdFontFamily::Style popupFontFamily = m.popup.textBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int y = static_cast<int>(renderer.getScreenHeight() * m.popup.topOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRounded = m.popup.cornerRadius > 0;
  if (useRounded) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             m.popup.cornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, m.popup.cornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }
  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + m.popup.textBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, m.popup.textInverted, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& m = *data;
  const int barHeight = m.popup.progress.barHeight;
  const int barWidth = std::max(0, layout.width - m.popup.marginX * 2);
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - m.popup.marginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  const int scaledProgress = m.popup.progress.clampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;
  if (m.popup.progress.drawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, m.popup.progress.outlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, m.popup.progress.fillInverted);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ============================================================================
// Status bar
// ============================================================================

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom,
                              const int textYOffset) const {
  const auto& m = *data;
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;
  int progressTextWidth = 0;

  if (SETTINGS.statusBarBookProgressPercentage || SETTINGS.statusBarChapterPageCount) {
    char progressStr[32];
    if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(
        SMALL_FONT_ID,
        renderer.getScreenWidth() - m.statusBar.horizontalMargin - orientedMarginRight - progressTextWidth, textY,
        progressStr);
  }

  if (SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int progressBarMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom -
                             ((SETTINGS.statusBarProgressBarThickness + 1) * 2) - paddingBottom;
    size_t progress;
    if (SETTINGS.statusBarProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    renderer.fillRect(orientedMarginLeft, progressBarY, barWidth, ((SETTINGS.statusBarProgressBarThickness + 1) * 2),
                      true);
  }

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  if (SETTINGS.statusBarBattery) {
    drawBatteryLeft(renderer,
                    Rect{m.statusBar.horizontalMargin + orientedMarginLeft + 1, textY, m.battery.width, m.battery.height},
                    showBatteryPercentage);
  }

  int clockTextWidth = 0;
  if (SETTINGS.statusBarClock && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      const int clockX = renderer.getScreenWidth() - m.statusBar.horizontalMargin - orientedMarginRight -
                         progressTextWidth - (progressTextWidth > 0 ? 10 : 0) - clockTextWidth;
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
    }
  }

  if (!title.empty()) {
    textY -= textYOffset;
    const int renderableScreenWidth =
        renderer.getScreenWidth() - (m.statusBar.horizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;
    const int batterySize = SETTINGS.statusBarBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int clockReserve = clockTextWidth > 0 ? (clockTextWidth + 10) : 0;
    const int titleMarginRight = progressTextWidth + clockReserve + 30;
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = renderableScreenWidth - 2 * titleMarginLeftAdjusted;
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      availableTitleSpace = renderableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }
    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + m.statusBar.horizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

// ============================================================================
// Misc widgets
// ============================================================================

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& m = *data;
  auto truncated =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - m.layout.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncated.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& m = *data;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + m.layout.verticalSpacing;
  const int thickness = cursorMode ? m.textField.cursorThickness : m.textField.normalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + m.textField.lineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + m.textField.horizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + m.textField.lineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  const auto& m = *data;
  const int cr = m.keyboard.keyCornerRadius;
  const bool isSpecialKey = keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode ||
                            keyType == KeyboardKeyType::Del || keyType == KeyboardKeyType::Space ||
                            keyType == KeyboardKeyType::Ok || keyType == KeyboardKeyType::Disabled;

  if (isSelected) {
    if (inactiveSelection) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
      }
    } else if (keyType == KeyboardKeyType::Disabled) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      }
    } else {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::Black);
      } else {
        renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      }
    }
  } else {
    if (m.keyboard.fillUnselected) {
      if (keyType == KeyboardKeyType::Disabled) {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
        } else {
          renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
        }
      } else {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::White);
        } else {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
        }
      }
    }
    const bool shouldDrawOutline =
        (m.keyboard.drawSpecialOutlineWhenUnselected && isSpecialKey) || m.keyboard.outlineAllUnselected;
    if (shouldDrawOutline) {
      if (cr > 0) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cr, true);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
      }
    }
  }

  const bool invert = isSelected && !inactiveSelection;
  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }
  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = std::max(m.keyboard.minArrowHeadSize, arrowLen / 2);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }
  if (label == nullptr || label[0] == '\0') return;

  const bool hasSecondary = secondaryLabel != nullptr && secondaryLabel[0] != '\0';
  const int itemWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, label, !invert);
  if (hasSecondary) {
    const int secWidth = renderer.getTextWidth(SMALL_FONT_ID, secondaryLabel);
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - secWidth - m.keyboard.secondaryLabelRightPadding,
                      rect.y + m.keyboard.secondaryLabelTopPadding, secondaryLabel, !invert);
  }
}

bool BaseTheme::showsFileIcons() const { return data->showsFileIcons; }
