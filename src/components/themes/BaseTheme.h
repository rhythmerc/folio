#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;
struct ThemeData;

struct Rect {
  int x;
  int y;
  int width;
  int height;
};

// A bare anchor point. Used where a caller wants to pin one corner of a
// floating element (e.g. a popup) without implying any width/height.
struct Position {
  int x;
  int y;
};

struct TabInfo {
  const char* label;
  bool selected;
};

// Forward declaration — ThemeData (in ThemeData.h) holds all theme fields
// directly, including layout/sizing fields that were previously grouped
// under a separate ThemeMetrics struct.

enum UIIcon { None = 0, Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Wifi, Hotspot, Bookmark };

enum class KeyboardKeyType { Normal, Shift, Mode, Space, Del, Ok, Disabled };

enum class FontRole {
  Title,    // Display titles ("Library.", page headers)
  Heading,  // Section headings, button-menu primary labels
  Body,     // Standard UI text (settings rows, list titles)
  Caption,  // Secondary text below the body (subtitles, author lines, hints)
  Accent,   // Italic accents, meta lines, breadcrumb / status chrome

  BodyCompact,
  CaptionCompact,
  AccentCompact,

  BodyLarge,  // 12pt UI chrome (UI_12); larger than Body, smaller than Title
};

// The single concrete theme implementation. All four built-in themes share
// this class; they differ only in the `ThemeData` instance pointed to by
// `data`. To add a new theme, declare a new `ThemeData` instance and add it
// to UITheme::setTheme().
class BaseTheme {
 public:
  // Default-construct binds to the Classic theme.
  BaseTheme();
  // Bind to a specific `ThemeData`. Keep the pointer valid for the theme's
  // lifetime — built-in themes live in flash and never move.
  explicit BaseTheme(const ThemeData* data);

  const ThemeData* getData() const { return data; }
  void setData(const ThemeData* newData) { data = newData; }

  // Convenience accessor for the theme's id (used as the SD font registry key).
  const char* themeName() const;

  // Maps a semantic role to a concrete font ID. Consults the SD font registry
  // first via the theme's id; falls back to the embedded font listed in
  // ThemeData. Static overload lets callers (e.g. LibraryActivity) resolve
  // roles against any ThemeData without holding a theme reference.
  int getFontForRole(FontRole role) const;
  static int resolveFontRole(const ThemeData& data, FontRole role);

  // Component drawing methods --------------------------------------------
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const;
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  int getListPageItems(int contentHeight, bool hasSubtitle) const;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int index)>& rowIcon = nullptr,
                const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& rowIsHeader = nullptr,
                const std::function<bool(int index)>& rowBold = nullptr, bool valueMetaStyle = false) const;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr, bool skipDecoration = false) const;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel = nullptr) const;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress) const;
  void drawStatusBar(GfxRenderer& renderer, float bookProgress, int currentPage, int pageCount, std::string title,
                     int paddingBottom = 0, int textYOffset = 0, bool fillMargin = true,
                     bool isPageBookmarked = false) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const;
  void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, bool isSelected,
                       const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                       bool inactiveSelection = false) const;
  bool showsFileIcons() const;

  // Shared primitives, available to any caller -----------------------------
  //
  // Promoted from FolioTheme so LibraryActivity (and any other widget that
  // wants the Folio visual language regardless of the active theme) can use
  // the same primitives without depending on a specific theme class.

  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

  // Selection rectangle primitive — generic building block parameterized
  // by fill and border style. Themes (and theme-adaptive activities like
  // Library) compose their selection treatment from these two axes.
  enum class SelectionFill : uint8_t {
    None,       // no background fill
    Solid,      // solid black fill (text should invert)
    LightGray,  // dithered light-gray wash (text stays black)
  };
  enum class SelectionBorder : uint8_t {
    None,      // no border stroke
    Single,    // 1-2 px ink outline
    Double,    // outer 2 px ink + 1 px white gap + inner 2 px ink (Folio)
    Brackets,  // diagonal corner brackets only (TL + BR)
  };
  // Renders a selection treatment by composition. Borders are drawn after
  // fills so a Double border can sit cleanly over a fill. The Brackets
  // border combines naturally with Double (Folio's signature) or stands
  // alone for a minimal accent.
  static void drawSelectionRect(const GfxRenderer& renderer, Rect rect, SelectionFill fill, SelectionBorder border,
                                int cornerRadius = 0);

  // Theme-aware selection treatment, split into two passes so callers can
  // bracket their content drawing correctly:
  //   1. drawSelectionBackground(rect)  — fills (drawn BEFORE content so
  //                                        cover bitmaps and text paint on
  //                                        top of the wash)
  //   2. drawSelectionForeground(rect)  — borders / brackets (drawn AFTER
  //                                        content so the frame sits on
  //                                        top)
  // A theme that uses only fills (Lyra's RoundedFill) is a no-op for
  // foreground; a theme that uses only borders (Folio's LayeredFrame) is
  // a no-op for background. Callers always call both — the no-ops are
  // free.
  void drawSelectionBackground(const GfxRenderer& renderer, Rect rect) const;
  void drawSelectionForeground(const GfxRenderer& renderer, Rect rect) const;

  // Folio-style selection: backwards-compat alias for drawSelectionRect
  // with Double border + Brackets. Kept for callers that want the Folio
  // look regardless of active theme.
  static void drawSelectionFrame(const GfxRenderer& renderer, Rect rect);
  static void drawCornerBrackets(const GfxRenderer& renderer, Rect rect, int armPx = 8, int strokePx = 2);

 private:
  const ThemeData* data;
};
