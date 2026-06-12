#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"  // Rect, UIIcon

class GfxRenderer;

// A single row in a List. Carries its own content and an optional action.
// Unlike the deprecated GUI.drawList (which demanded a fistful of index-keyed
// lambdas), each row owns its data directly, so building a list is just
// pushing ListItems onto a vector.
struct ListItem {
  std::string title;
  std::string subtitle;             // optional — empty hides it
  std::string value;                // optional right-aligned meta column
  std::optional<UIIcon> icon;       // optional leading icon
  bool dimmed = false;              // checker-dithered (e.g. unavailable)
  bool header = false;              // non-selectable section divider
  bool bold = false;                // bold title face
  std::function<void()> onSelect;   // fired by triggerSelected()
};

// Selectable, scrollable list of ListItems. Owns the selected index and
// handles navigation (skipping header rows) and dispatch. Rendering reuses the
// theme's list primitive so styling stays consistent across the app.
class List {
 public:
  List() = default;
  explicit List(std::vector<ListItem> items, int selectedIndex = 0);

  void setItems(std::vector<ListItem> items);
  const std::vector<ListItem>& items() const { return items_; }
  size_t size() const { return items_.size(); }

  int selectedIndex() const { return selected_; }
  void setSelectedIndex(int index);

  // Move selection to the previous/next selectable (non-header) row, clamped
  // at the ends. No-op when there is no selectable row in that direction.
  void up();
  void down();

  // Invoke the selected row's onSelect callback, if it has one.
  void triggerSelected();

  void render(const GfxRenderer& renderer, Rect rect) const;

  // Row-styling toggles, mirroring the theme primitive:
  //   highlightValue — invert the value chip on the selected row
  //   valueMetaStyle — render values smaller + italic (the .row-meta look)
  bool highlightValue = false;
  bool valueMetaStyle = false;

 private:
  std::vector<ListItem> items_;
  int selected_ = 0;

  bool isSelectable(int index) const;
};
