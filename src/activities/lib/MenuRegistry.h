#pragma once

#include <Bitmap1Bit.h>
#include <functional>
#include <optional>
#include <string>
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

struct MenuRegistryEntry {
  using Icon = Bitmap1Bit;
  Icon icon;
  std::string name;
  std::optional<std::function<void()>> onPress;
  std::vector<PopupMenuEntry> popupItems;
};

// Per-activity GlobalMenu opt-in + config. An activity returns std::nullopt to
// disable the sidebar, or a config to enable it.
struct GlobalMenuConfig {
  // Clear the global font glyph caches when the menu closes. Reader-only: it
  // reclaims the RAM the UI fonts warmed so the reader's grayscale framebuffer
  // snapshot has headroom again. Cheap screens leave this false.
  bool clearFontCacheOnClose = false;
};
