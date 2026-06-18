#include "CascadingPopupMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

bool CascadingPopupMenu::isBranch(const PopupMenuEntry& e) { return !e.children.empty(); }

void CascadingPopupMenu::configure(std::function<std::vector<PopupMenuEntry>()> builder,
                                   std::function<void()> requestUpdate) {
  builder_ = std::move(builder);
  requestUpdate_ = std::move(requestUpdate);
}

void CascadingPopupMenu::notifyUpdate() {
  if (requestUpdate_) requestUpdate_();
}

void CascadingPopupMenu::pushLevel(const std::vector<PopupMenuEntry>& entries, std::optional<uint8_t> selection) {
  Level level;
  level.entries = &entries;
  level.menu.setItemCount(static_cast<int>(entries.size()), selection);
  stack_.push_back(std::move(level));
}

void CascadingPopupMenu::open(std::optional<uint8_t> initialSelection) {
  open_ = true;
  stack_.clear();
  rootInitial_ = initialSelection;
  entries_ = builder_ ? builder_() : std::vector<PopupMenuEntry>{};
  // Open as an unselected preview; the first activate() reveals rootInitial_.
  pushLevel(entries_, std::nullopt);
  notifyUpdate();
}

void CascadingPopupMenu::close() {
  open_ = false;
  stack_.clear();
}

bool CascadingPopupMenu::isEntered() const {
  return open_ && !stack_.empty() && stack_.back().menu.selectedIndex().has_value();
}

const PopupMenuEntry* CascadingPopupMenu::focusedEntry() const {
  if (!open_ || stack_.empty()) return nullptr;
  const Level& level = stack_.back();
  const std::optional<uint8_t> sel = level.menu.selectedIndex();
  if (!sel.has_value() || *sel >= level.entries->size()) return nullptr;
  return &(*level.entries)[*sel];
}

void CascadingPopupMenu::rebuildPreservingPath() {
  // Snapshot the descended path before the tree is replaced.
  std::vector<std::optional<uint8_t>> path;
  path.reserve(stack_.size());
  for (const Level& level : stack_) path.push_back(level.menu.selectedIndex());

  entries_ = builder_ ? builder_() : std::vector<PopupMenuEntry>{};
  stack_.clear();
  if (path.empty()) {
    pushLevel(entries_, std::nullopt);
    return;
  }

  // Root, then re-descend while each saved row still names a branch.
  pushLevel(entries_, path[0]);
  for (size_t d = 1; d < path.size(); ++d) {
    const PopupMenuEntry* entry = focusedEntry();
    if (entry == nullptr || !isBranch(*entry)) break;
    pushLevel(entry->children, path[d]);
  }
}

void CascadingPopupMenu::moveUp() {
  if (!open_ || stack_.empty()) return;
  if (stack_.back().menu.moveUp()) notifyUpdate();
}

void CascadingPopupMenu::moveDown() {
  if (!open_ || stack_.empty()) return;
  if (stack_.back().menu.moveDown()) notifyUpdate();
}

void CascadingPopupMenu::back() {
  if (!open_ || stack_.empty()) return;
  if (stack_.size() > 1) {
    // Pop one submenu level; the parent keeps its selection (still entered).
    stack_.pop_back();
  } else {
    // At the root: de-select so the popup falls back to an unselected preview.
    // The caller (GlobalMenu) regains navigation; it closes the popup itself.
    stack_.back().menu.setSelectedIndex(std::nullopt);
  }
  notifyUpdate();
}

bool CascadingPopupMenu::activate() {
  if (!open_ || stack_.empty()) return false;

  // Activating a freshly-opened preview reveals the initial selection instead
  // of acting — the user steps in before navigating.
  PopupMenu& focused = stack_.back().menu;
  if (!focused.selectedIndex().has_value()) {
    focused.setSelectedIndex(rootInitial_.value_or(uint8_t{0}));
    notifyUpdate();
    return false;
  }

  const PopupMenuEntry* entry = focusedEntry();
  if (entry == nullptr) return false;

  if (isBranch(*entry)) {
    // Entering a submenu lands on its pre-selected row, or row 0 by default.
    pushLevel(entry->children, entry->initialSelectedChild.value_or(uint8_t{0}));
    notifyUpdate();
    return false;
  }

  if (entry->onSelected.has_value()) {
    const bool shouldClose = (*entry->onSelected)();
    if (!shouldClose) {
      // The leaf mutated state (e.g. the sort field); rebuild so the tree's
      // glyphs reflect it, keeping the user on the same row.
      rebuildPreservingPath();
    }
    notifyUpdate();
    return shouldClose;
  }
  return false;
}

int CascadingPopupMenu::panelHeight(int itemCount) {
  const auto& pm = GUI.getData()->popupMenu;
  return itemCount * pm.rowHeight + 2 * pm.borderThickness;
}

int CascadingPopupMenu::panelWidth(GfxRenderer& renderer, const std::vector<PopupMenuEntry>& entries) {
  const auto& pm = GUI.getData()->popupMenu;
  const int font = GUI.getFontForRole(pm.fontRole);
  int maxLabelW = 0;
  bool anyGlyph = false;
  for (const PopupMenuEntry& e : entries) {
    // A row paints a glyph when it's a branch (chevron) or carries one.
    if (isBranch(e) || e.glyph.has_value()) anyGlyph = true;
    if (e.label == nullptr || e.label[0] == '\0') continue;
    // Measure in the same weight PopupMenu renders the label (REGULAR);
    // measuring BOLD over-reserves and leaves slack on the trailing edge.
    maxLabelW = std::max(maxLabelW, renderer.getTextWidth(font, e.label));
  }
  // Layout per row: borderThickness | paddingX | label [… glyph | paddingX] | borderThickness.
  // Only reserve the glyph gutter when some row in this panel actually has one,
  // so glyphless panels (e.g. Book / Files) don't carry dead trailing space.
  // The tree is rebuilt on open, so whether a panel has glyphs is stable for its
  // lifetime — no runtime reflow to guard against. Matches PopupMenu's sizing.
  const int glyphReserve = anyGlyph ? (std::max(6, pm.rowHeight / 3) + pm.paddingX) : 0;
  return 2 * pm.borderThickness + 2 * pm.paddingX + maxLabelW + glyphReserve;
}

void CascadingPopupMenu::render(GfxRenderer& renderer, Anchor anchor, Rect area) const {
  if (!open_ || stack_.empty()) return;
  const auto& pm = GUI.getData()->popupMenu;
  const int areaTop = area.y;
  const int areaBottom = area.y + area.height;
  const int rightLimit = area.x + area.width;
  const size_t focused = stack_.size() - 1;

  // The focused branch's children render as a preview panel (no highlight) when
  // we haven't entered them yet.
  PopupMenu previewMenu;
  const std::vector<PopupMenuEntry>* previewEntries = nullptr;
  if (const PopupMenuEntry* entry = focusedEntry(); entry != nullptr && isBranch(*entry)) {
    previewEntries = &(entry->children);
    previewMenu.setItemCount(static_cast<int>(previewEntries->size()), std::nullopt);
  }
  const int panelCount = static_cast<int>(stack_.size()) + (previewEntries != nullptr ? 1 : 0);

  // Walk the panel chain left-to-right. The root pins a corner to `anchor`;
  // every subsequent panel cascades from its parent's selected row.
  int x = anchor.pos.x;
  int prevTop = 0;                 // top edge of the panel drawn last iteration
  std::optional<uint8_t> prevSel;  // its selected row — the one a child hangs off
  for (int p = 0; p < panelCount; ++p) {
    const bool isPreview = (p == static_cast<int>(stack_.size()));
    const std::vector<PopupMenuEntry>& entries = isPreview ? *previewEntries : *stack_[p].entries;
    const PopupMenu& menu = isPreview ? previewMenu : stack_[p].menu;
    const bool isFocused = (!isPreview && static_cast<size_t>(p) == focused);

    const int count = static_cast<int>(entries.size());
    if (count == 0) break;

    int w = panelWidth(renderer, entries);
    if (x + w + pm.shadowOffsetX > rightLimit) w = rightLimit - pm.shadowOffsetX - x;
    if (w < 2 * pm.borderThickness + pm.paddingX) break;
    const int h = panelHeight(count);

    // Vertical placement: the root pins a corner to `anchor` — top-left when the
    // panel fits below within `area`, otherwise bottom-left. A child aligns its
    // top with the parent's selected-row top when it still fits below, otherwise
    // aligns its bottom with the row. Everything is clamped into `area`.
    int y;
    if (p == 0) {
      const bool fitsBelow = anchor.pos.y + h + pm.shadowOffsetY <= areaBottom;
      y = (anchor.origin == AnchorOrigin::TopLeft && fitsBelow) ? anchor.pos.y : anchor.pos.y - h;
    } else if (!prevSel.has_value()) {
      y = prevTop;
    } else {
      const int rowTop = prevTop + pm.borderThickness + static_cast<int>(*prevSel) * pm.rowHeight;
      y = (rowTop + h + pm.shadowOffsetY <= areaBottom) ? rowTop : (rowTop + pm.rowHeight - h);
    }
    if (y + h + pm.shadowOffsetY > areaBottom) y = areaBottom - h - pm.shadowOffsetY;
    if (y < areaTop) y = areaTop;  // taller than area → clamp top, let shadow spill

    // Ancestors get a muted parent-row wash and no highlight; the focused panel
    // shows its selection; the preview shows none.
    int mutedRow = -1;
    if (!isFocused && !isPreview && menu.selectedIndex().has_value()) {
      mutedRow = static_cast<int>(*menu.selectedIndex());
    }

    const std::function<const char*(int)> labelFn = [&entries](int i) -> const char* { return entries[i].label; };
    const std::function<PopupMenu::Glyph(int)> glyphFn = [&entries](int i) -> PopupMenu::Glyph {
      const PopupMenuEntry& e = entries[i];
      if (isBranch(e)) return PopupMenu::Glyph::ChevronRight;
      return e.glyph.value_or(PopupMenu::Glyph::None);
    };
    menu.render(renderer, Rect{x, y, w, h}, /*showSelection=*/isFocused, labelFn, glyphFn, mutedRow);

    prevTop = y;
    prevSel = menu.selectedIndex();
    x += w + pm.panelGap;
  }
}

MappedInputManager::Labels CascadingPopupMenu::getButtonLabels(const MappedInputManager& mappedInput) const {
  const PopupMenuEntry* entry = focusedEntry();
  const bool confirmEnters = entry != nullptr && isBranch(*entry);
  const char* back = tr(STR_BACK);
  const char* confirm = confirmEnters ? tr(STR_ENTER) : tr(STR_SELECT);
  return mappedInput.mapLabels(back, confirm, "", "");
}

