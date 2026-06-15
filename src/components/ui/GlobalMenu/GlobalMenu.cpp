#include "GlobalMenu.h"
#include "components/UITheme.h"
#include "components/ui/UIPage/UIPage.h"

static constexpr int navWidth = 64;
static constexpr int navPaddingX = 16;
static constexpr int navPaddingY = 24;
static constexpr int iconSize = 32;

bool GlobalMenu::loop() {
  // Toggle on the press edge — the same edge activities consume to open their
  // own back-driven UI (e.g. the library popup). Reacting on release lets the
  // activity see the press first and act on it before the menu opens.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    opened = !opened;
    if (!opened) {
      activityEntry.reset();
    }
    return true;
  }
  return false;
}

void GlobalMenu::render() {
  const auto& td = *GUI.getData();

  // background overlay
  renderer.fillRectDither<true>(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), Color::LightGray);

  const auto selected = getSelectedEntry();

  const auto body = flex::inset(
    UIPage::render(
      renderer,
      "Menu",
      selected.has_value()
        ? selected.value().name.c_str()
        : "",
      MappedInputManager::Labels{}
    ),
    flex::Padding{ flex::xy(td.globalMenu.paddingX, td.globalMenu.paddingY) }
  );

  const auto sections = flex::Hstack(
      body,
      {
        flex::fixed(navWidth),
        flex::grow()
      }
  );

  const auto nav = sections[0];
  renderNavBody(nav);
  renderNavItems(nav);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayBuffer();
}

std::vector<MenuRegistryEntry> GlobalMenu::getEntries() {
  std::vector<MenuRegistryEntry> entries;
  entries.reserve(5);

  if(activityEntry.has_value()) {
    entries.emplace_back(activityEntry.value());
  }

  return entries;
}

std::optional<MenuRegistryEntry> GlobalMenu::getSelectedEntry() {
  auto entries = getEntries();

  if(selectedIndex >= entries.size()) {
    return std::nullopt;
  }

  return entries[selectedIndex];
}

void GlobalMenu::renderNavBody(Rect nav) {
  const auto navShadow = flex::offset(nav, 5, 5);

  renderer.fillRect(navShadow.x, navShadow.y, navShadow.width, navShadow.height);

  renderer.fillRect(nav.x, nav.y, nav.width, nav.height, false);
  renderer.drawRect(nav.x, nav.y, nav.width, nav.height, true);
}

void GlobalMenu::renderNavItems(Rect body) {
  auto entries = getEntries();

  for (uint8_t i = 0; const MenuRegistryEntry& entry : entries) {
    bool selected = i == selectedIndex;

    Rect area = {
      .x = body.x,
      .y = body.y,
      .width = body.width,
      .height = entry.icon.height + (2 * navPaddingY)
    };

    if (selected) {
      renderer.fillRect(area.x, area.y, area.width, area.height, true);
    }

    Rect target = flex::center(area, entry.icon.width, entry.icon.height);
    renderer.drawIcon(entry.icon, target.x, target.y, selected);

    body = { 
      .x = body.x,
      .y = body.y + area.height,
      .width = body.width,
      .height = body.height - area.height
    }; // shift the body down by the area dimensions to correctly derive the next target

    ++i;
  }
}
