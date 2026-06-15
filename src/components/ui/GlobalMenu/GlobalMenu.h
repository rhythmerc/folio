#pragma once

#include <optional>
#include <utility>
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/lib/MenuRegistry.h"
#include "components/themes/BaseTheme.h"

class GlobalMenu {
  private:
    std::optional<MenuRegistryEntry> activityEntry;
    bool opened = false;
    GfxRenderer& renderer;
    MappedInputManager& mappedInput;

    uint8_t selectedIndex = 0;

    void renderNavBody(Rect area);
    void renderNavItems(Rect area);

    std::vector<MenuRegistryEntry> getEntries();
    std::optional<MenuRegistryEntry> getSelectedEntry();

  public:
    GlobalMenu(GfxRenderer& renderer, MappedInputManager& mappedInput) :
      renderer(renderer), mappedInput(mappedInput) {}

    bool isOpen() const { return opened; }
    bool hasActivityEntry() const { return activityEntry.has_value(); }

    // Supply the current activity's menu data. Called by the manager on the
    // closed->open transition, while the activity is still alive.
    void setEntry(std::optional<MenuRegistryEntry> entry) { activityEntry = std::move(entry); }

    // Handle input for the menu. Returns true if the open/closed state changed
    // this frame, so the caller can trigger exactly one e-ink refresh.
    bool loop();

    void render();
};
