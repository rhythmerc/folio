#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "components/themes/BaseTheme.h"  // for Rect
#include "components/ui/PopupMenu/PopupMenu.h"

// A single row in a cascading popup menu. Recursive: a row is either a
//   - branch: `children` is set and non-empty. Activating it descends into a
//     submenu panel that cascades to the right. `initialSelectedChild`
//     pre-selects a row on entry (e.g. the active sort field); omit for row 0.
//     Branches paint an automatic chevron, so leave `glyph` unset for them.
//   - leaf: `onSelected` fires when the row is activated. The callback returns
//     `true` to close the whole popup, `false` to leave it open (the menu then
//     rebuilds, refreshing any state-dependent glyphs). The callback owns its
//     own side effects (navigation, settings writes, …).
struct PopupMenuEntry {
  const char* label = "";
  std::optional<PopupMenu::Glyph> glyph;
  std::optional<uint8_t> initialSelectedChild;
  std::vector<PopupMenuEntry> children;
  std::optional<std::function<bool()>> onSelected;
};

// An arbitrary-depth cascading menu over a tree of PopupMenuEntry. The tree is
// produced by a builder callback so state-dependent glyphs / pre-selections are
// recomputed each time the menu opens (and after any non-closing leaf action) —
// no per-render callbacks, no stale data.
//
// The menu owns its own redraws: every input handler calls the configured
// requestUpdate hook when it changes visible state. Callers route a button
// press to the matching method and return immediately.
//
// The cascade owns panel chrome / layout, cross-level navigation, the
// muted-parent indicator, and the chevron-vs-arrow glyph derivation. Each
// entry owns what its row *means* (label + leaf action).
class CascadingPopupMenu {
 public:
  enum AnchorOrigin {
    BottomLeft,
    TopLeft
  };

  struct Anchor{
    AnchorOrigin origin;
    Position pos;
  };

  CascadingPopupMenu() = default;

  // Install the menu. `builder` produces the entry tree (re-run on open and
  // after any non-closing leaf action). `requestUpdate` is invoked whenever the
  // menu changes visible state. Does not open the menu.
  void configure(std::function<std::vector<PopupMenuEntry>()> builder, std::function<void()> requestUpdate);

  // Open onto the root panel with NO row selected (a preview). The root row
  // revealed by the first activate() is `initialSelection` (default row 0).
  void open(std::optional<uint8_t> initialSelection = uint8_t{0});
  void close();
  bool isOpen() const { return open_; }

  // True once the user has stepped into the popup — i.e. the focused panel has
  // a selected row. False while the popup is an unselected preview (just
  // opened, or backed out to the root). Callers route navigation to the popup
  // only while it's entered.
  bool isEntered() const;

  // Input. The menu calls requestUpdate() itself when state changed; callers
  // delegate and return.
  void moveUp();
  void moveDown();
  void back();  // pop one level, or de-select the root (back out to preview)
  // Confirm: reveal the initial selection on a preview, descend into a branch,
  // or fire a leaf's onSelected. Returns true when the caller should close the
  // whole menu (a leaf's onSelected requested it).
  bool activate();

  // Render the cascade (no-op when closed) within `area`. The root panel's left
  // edge sits at `anchor.x`; `anchor.y` pins one of its corners — the panel
  // grows downward from `anchor` (top-left) when it fits below within `area`,
  // otherwise upward (bottom-left). Child panels cascade rightward, bounded by
  // `area`'s right and bottom edges.
  void render(GfxRenderer& renderer, Anchor anchor, Rect area) const;

  // Button hints for the open popup (back: Close at root / Back in a submenu,
  // confirm: Enter on a branch / Select on a leaf; left/right blank), routed
  // through mappedInput so physical remapping applies.
  MappedInputManager::Labels getButtonLabels(const MappedInputManager& mappedInput) const;

 private:
  // One open panel. `entries` points into the current `entries_` tree; `menu`
  // owns this panel's selection + item count.
  struct Level {
    const std::vector<PopupMenuEntry>* entries = nullptr;
    PopupMenu menu;
  };

  bool open_ = false;
  std::function<std::vector<PopupMenuEntry>()> builder_;
  std::function<void()> requestUpdate_;
  std::vector<PopupMenuEntry> entries_;  // current tree (built by builder_)
  std::vector<Level> stack_;             // stack_.back() == focused panel
  // Root row revealed by the first activate() on a freshly-opened preview.
  std::optional<uint8_t> rootInitial_;

  // The selected entry in the focused panel, or nullptr when nothing is
  // selected / the menu is closed.
  const PopupMenuEntry* focusedEntry() const;
  static bool isBranch(const PopupMenuEntry& e);

  // Push a panel for `entries` selected at `selection` onto the stack.
  void pushLevel(const std::vector<PopupMenuEntry>& entries, std::optional<uint8_t> selection);
  // Re-run builder_ and re-descend the saved per-level selection so the user
  // stays in the same submenu on the same row (clamped to the new sizes).
  void rebuildPreservingPath();
  void notifyUpdate();

  static int panelHeight(int itemCount);
  static int panelWidth(GfxRenderer& renderer, const std::vector<PopupMenuEntry>& entries);
};
