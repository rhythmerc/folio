#include "List.h"

#include <algorithm>

#include <GfxRenderer.h>

#include "components/UITheme.h"

List::List(std::vector<ListItem> items, int selectedIndex) : items_(std::move(items)) {
  setSelectedIndex(selectedIndex);
}

void List::setItems(std::vector<ListItem> items) {
  items_ = std::move(items);
  setSelectedIndex(selected_);
}

bool List::isSelectable(int index) const {
  return index >= 0 && index < static_cast<int>(items_.size()) && !items_[index].header;
}

void List::setSelectedIndex(int index) {
  if (items_.empty()) {
    selected_ = 0;
    return;
  }
  const int last = static_cast<int>(items_.size()) - 1;
  selected_ = std::min(std::max(index, 0), last);
}

void List::up() {
  const int start = selected_ == 0
    ? items_.size() - 1
    : selected_ - 1;

  for (int i = start; i >= 0; --i) {
    if (isSelectable(i)) {
      selected_ = i;
      return;
    }
  }
}

void List::down() {
  const int count = static_cast<int>(items_.size());
  const int start = selected_ == (items_.size() - 1)
    ? 0
    : selected_ + 1;

  for (int i = start; i < count; ++i) {
    if (isSelectable(i)) {
      selected_ = i;
      return;
    }
  }
}

void List::triggerSelected() {
  if (isSelectable(selected_) && items_[selected_].onSelect) {
    items_[selected_].onSelect();
  }
}

void List::render(const GfxRenderer& renderer, Rect rect) const {
  if (items_.empty()) return;
  const auto &td = *GUI.getData();

  // The theme primitive keys layout off which callbacks are non-null (e.g. a
  // present rowSubtitle makes every row taller). Only hand it the callbacks a
  // row actually uses, so a value-less / icon-less list lays out tight.
  bool anySubtitle = false;
  bool anyIcon = false;
  bool anyValue = false;
  bool anyDimmed = false;
  bool anyHeader = false;
  bool anyBold = false;
  for (const auto& item : items_) {
    anySubtitle |= !item.subtitle.empty();
    anyIcon |= item.icon.has_value();
    anyValue |= !item.value.empty();
    anyDimmed |= item.dimmed;
    anyHeader |= item.header;
    anyBold |= item.bold;
  }

  const std::function<std::string(int)> rowTitle = [this](int i) { return items_[i].title; };
  std::function<std::string(int)> rowSubtitle;
  std::function<UIIcon(int)> rowIcon;
  std::function<std::string(int)> rowValue;
  std::function<bool(int)> rowDimmed;
  std::function<bool(int)> rowIsHeader;
  std::function<bool(int)> rowBold;
  if (anySubtitle) rowSubtitle = [this](int i) { return items_[i].subtitle; };
  if (anyIcon) rowIcon = [this](int i) { return items_[i].icon.value_or(UIIcon::File); };
  if (anyValue) rowValue = [this](int i) { return items_[i].value; };
  if (anyDimmed) rowDimmed = [this](int i) { return items_[i].dimmed; };
  if (anyHeader) rowIsHeader = [this](int i) { return items_[i].header; };
  if (anyBold) rowBold = [this](int i) { return items_[i].bold; };

  // lists apply their own padding, so we must expand the rectangle horizontally by contentSidePadding if we're already inset
  const Rect listRect = rect.x != 0
    ? Rect{
        .x = rect.x - td.layout.contentSidePadding,
        .y = rect.y,
        .width = rect.width + (2 * td.layout.contentSidePadding),
        .height = rect.height
      }
    : std::move(rect);

  GUI.drawList(renderer, listRect, static_cast<int>(items_.size()), selected_, rowTitle, rowSubtitle, rowIcon, rowValue,
               highlightValue, rowDimmed, rowIsHeader, rowBold, valueMetaStyle);
}
