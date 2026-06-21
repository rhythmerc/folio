#include <I18n.h>

#include "GlobalMenu.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/ui/UIPage/UIPage.h"

static constexpr int navWidth = 64;

void GlobalMenu::closeMenu() {
  opened = false;
  selectedIndex = 0;
  popup_.close();
  // The press that closed the menu is still held; don't let its release fall
  // through to the activity now regaining control.
  mappedInput.suppressHeldConsumedReleases();
}

void GlobalMenu::syncPopupToSelection() {
  auto entry = getSelectedEntry();
  if (entry.has_value() && !entry.value().popupItems.empty()) {
    popup_.open();  // unselected preview; the first Confirm reveals the selection
  } else {
    popup_.close();
  }
}

bool GlobalMenu::loop() {
  MappedInputManager::Button up;
  MappedInputManager::Button down;
  MappedInputManager::Button left;
  MappedInputManager::Button right;

  auto orientation = renderer.getOrientation();

  if (!opened) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      opened = true;
      selectedIndex = 0;
      syncPopupToSelection();
      return true;
    }

    return false;
  }
  
  // While the popup is entered, navigation routes to it.
  if (popup_.isEntered()) {
    if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Down, orientation)) {
      popup_.moveDown();
      return true;
    }
    if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Up, orientation)) {
      popup_.moveUp();
      return true;
    }
    if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Left, orientation)) {
      popup_.back();  // pop a submenu level, or de-select the root (→ preview)
      return true;
    }
    if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Right, orientation)) {
      if (popup_.activate()) closeMenu();  // a leaf requested the whole menu close
      return true;
    }
    return false;
  }

  // Nav mode: move the selection (refreshing the preview popup), or act on it.
  if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Down, orientation)) {
    const auto size = getTopEntries().size() + getBottomEntries().size();
    selectedIndex = (selectedIndex == size - 1) ? 0 : selectedIndex + 1;
    syncPopupToSelection();
    return true;
  }

  if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Up, orientation)) {
    const auto size = getTopEntries().size() + getBottomEntries().size();
    selectedIndex = (selectedIndex == 0) ? size - 1 : selectedIndex - 1;
    syncPopupToSelection();
    return true;
  }

  if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Left, orientation)) {
    closeMenu(); 
    return true;
  }

  if (mappedInput.wasPressed(MappedInputManager::DirectionalButton::Right, orientation)) {
    auto current = getSelectedEntry();
    if (current.has_value() && !current.value().popupItems.empty()) {
      popup_.activate();  // reveal the initial selection — enters the popup
      return true;
    }
    if (current.has_value() && current.value().onPress.has_value()) {
      (current.value().onPress.value())();
      closeMenu();
      return true;
    }
    return false;
  }

  return false;
}

std::optional<MenuRegistryEntry> GlobalMenu::getSelectedEntry() {
  auto i = selectedIndex;

  auto top = getTopEntries();
  if(i < top.size()) {
    return top[i];
  }
  
  // attempt to index into bottom
  i -= top.size(); 
  auto bottom = getBottomEntries();

  if(i < bottom.size()) {
    return bottom[i];
  }

  // before we fail out, fall back to first top entry if possible
  if(top.size() > 0) {
    selectedIndex = 0;
    return top[0];
  }

  // before we fail out, bail out to bottom top entry if possible
  if(bottom.size() > 0) {
    selectedIndex = 0;
    return bottom[0];
  }

  return std::nullopt;
}

void GlobalMenu::render() {
  if(!opened) {
    return;
  }

  const auto& td = *GUI.getData();

  auto selected = getSelectedEntry();
  auto subtitle = selected.has_value()
    ? selected.value().name
    : "";

  MappedInputManager::Labels labels;
  if (popup_.isEntered()) {
    labels = popup_.getButtonLabels(mappedInput);
  } else {
    const char* confirm = "";
    if (selected.has_value()) {
      if (!selected.value().popupItems.empty()) {
        confirm = tr(STR_ENTER);
      } else if (selected.value().onPress.has_value()) {
        confirm = tr(STR_SELECT);
      }
    }
    labels = mappedInput.mapLabels(tr(STR_EXIT), confirm, "", "");
  }

  constexpr int gap = 10;
  auto body = UIPage::render(
      renderer, 
      tr(STR_MENU_LABEL), 
      subtitle.c_str(),
      labels,
      flex::Padding{ .bottom = gap }
  );

  const auto sections = flex::Hstack(
      body,
      {
        flex::fixed(navWidth + 2 * td.globalMenu.paddingX),
        flex::grow()
      },
      0,
      flex::xy(0, td.globalMenu.paddingY) 
  );

  // Inset the nav bar frame within its section (1: nav-area padding).
  const auto nav = flex::inset(sections[0], flex::xy(td.globalMenu.paddingX, 0));
  const auto popupArea = sections[1];

  renderNavBody(nav);
  renderNavItems(nav);

  if (popup_.isOpen()) {
    const Rect slot = selectedSlotRect(nav);
    const bool isBottom = selectedIndex >= getTopEntries().size();

    const CascadingPopupMenu::Anchor anchor{ 
      .origin = isBottom ? CascadingPopupMenu::AnchorOrigin::BottomLeft : CascadingPopupMenu::AnchorOrigin::TopLeft,
      .pos = { 
        nav.x + nav.width + td.globalMenu.popupGap, 
        isBottom ? slot.y + slot.height : slot.y
      }
    };

    popup_.render(renderer, anchor, popupArea);
  }

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayBuffer();
}

Rect GlobalMenu::selectedSlotRect(Rect nav) {
  const auto top = getTopEntries();
  const auto bottom = getBottomEntries();
  const int slotSize = nav.width;

  if (selectedIndex < top.size()) {
    return Rect{nav.x, nav.y + static_cast<int>(selectedIndex) * slotSize, slotSize, slotSize};
  }

  // Bottom entries are anchored to the bottom of the nav, index 0 highest.
  const int bi = static_cast<int>(selectedIndex - top.size());
  const int totalBottomHeight = static_cast<int>(bottom.size()) * slotSize;
  const int bottomBaseY = nav.y + nav.height - totalBottomHeight;
  return Rect{nav.x, bottomBaseY + bi * slotSize, slotSize, slotSize};
}

void GlobalMenu::renderNavBody(Rect nav) {
  const auto& td = *GUI.getData();
  const int radius = td.globalMenu.cornerRadius;

  if (td.globalMenu.shadowOffsetX != 0 || td.globalMenu.shadowOffsetY != 0) {
    const Rect sh = flex::offset(nav, td.globalMenu.shadowOffsetX, td.globalMenu.shadowOffsetY);
    renderer.fillRoundedRect(sh.x, sh.y, sh.width, sh.height, radius, Color::Black);
  }

  renderer.fillRoundedRect(nav.x, nav.y, nav.width, nav.height, radius, Color::White);
  if (td.globalMenu.borderWidth > 0) {
    renderer.drawRoundedRect(nav.x, nav.y, nav.width, nav.height, td.globalMenu.borderWidth, radius, true);
  }
}

void GlobalMenu::renderNavItems(Rect body) {
  const auto entries = getTopEntries();
  const auto bottomEntries = getBottomEntries();

  const uint16_t slotSize = body.width;

  // --- Main entries (stacked from top) ---
  for (uint8_t i = 0; i < entries.size(); ++i) {
    const Rect slot = { .x = body.x, .y = body.y + i * slotSize, .width = slotSize, .height = slotSize };
    renderSlot(entries[i], slot, i == selectedIndex);
  }

  // --- Bottom entries (anchored to bottom, top-to-bottom: index 0 highest) ---
  const uint16_t totalBottomHeight = bottomEntries.size() * slotSize;
  const uint16_t bottomBaseY = body.y + body.height - totalBottomHeight;

  for (size_t i = 0; i < bottomEntries.size(); ++i) {
    const Rect slot = {
      .x = body.x,
      .y = static_cast<int>(bottomBaseY + i * slotSize),
      .width = slotSize,
      .height = slotSize
    };
    renderSlot(bottomEntries[i], slot, (i + entries.size()) == selectedIndex);
  }
}

void GlobalMenu::renderSlot(const MenuRegistryEntry& entry, Rect slot, bool selected) {
  const auto& td = *GUI.getData();
  bool iconInverted = false;

  if (selected) {
    // Indicator inset from the slot (2), with fill (6), optional border (6) and radius (7).
    const Rect ind = flex::inset(slot, flex::all(td.globalMenu.selectionPadding));
    if (td.globalMenu.selectionFill != Color::Clear) {
      renderer.fillRoundedRect(ind.x, ind.y, ind.width, ind.height,
                               td.globalMenu.selectionCornerRadius, td.globalMenu.selectionFill);
    }
    if (td.globalMenu.selectionBorderWidth > 0) {
      renderer.drawRoundedRect(ind.x, ind.y, ind.width, ind.height,
                               td.globalMenu.selectionBorderWidth, td.globalMenu.selectionCornerRadius, true);
    }
    // White icon only over a dark fill; black icon over clear/light fills.
    iconInverted = td.globalMenu.selectionFill == Color::Black || td.globalMenu.selectionFill == Color::DarkGray;
  }

  const Rect target = flex::center(slot, entry.icon.width, entry.icon.height);
  renderer.drawIcon(entry.icon, target.x, target.y, iconInverted);
}
