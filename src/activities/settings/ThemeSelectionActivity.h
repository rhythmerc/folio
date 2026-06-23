#pragma once

#include "activities/Activity.h"
#include "components/ui/List/List.h"
#include "util/ButtonNavigator.h"

class UiThemeLoader;

// Full-screen picker for the active UI theme: built-in "Folio" plus every
// discovered SD .cptheme. Selecting one writes SETTINGS.uiTheme/sdThemeName and
// returns; the caller saves + reloads the theme. Mirrors FontSelectionActivity.
class ThemeSelectionActivity final : public Activity {
 public:
  explicit ThemeSelectionActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const UiThemeLoader* loader
  );

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void buildList();

  const UiThemeLoader* loader_;
  ButtonNavigator buttonNavigator_;
  List list_;
};
