#include "ThemeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "UiThemeLoader.h"
#include "components/ui/UIPage/UIPage.h"

ThemeSelectionActivity::ThemeSelectionActivity(
  GfxRenderer& renderer, MappedInputManager& mappedInput, const UiThemeLoader* loader
)
    : Activity("ThemeSelect", renderer, mappedInput), loader_(loader) {}

void ThemeSelectionActivity::onEnter() {
  Activity::onEnter();
  buildList();
  requestUpdate();
}

void ThemeSelectionActivity::onExit() { Activity::onExit(); }

void ThemeSelectionActivity::buildList() {
  const bool sdActive =
    SETTINGS.uiTheme == CrossPointSettings::SD_THEME && SETTINGS.sdThemeName[0] != '\0';

  std::vector<ListItem> items;
  int current = 0;

  ListItem folio;
  folio.title = tr(STR_THEME_FOLIO);
  folio.value = sdActive ? "" : tr(STR_SELECTED);
  folio.onSelect = [this] {
    SETTINGS.uiTheme = CrossPointSettings::FOLIO;
    SETTINGS.sdThemeName[0] = '\0';
    finish();
  };
  items.push_back(std::move(folio));

  if (loader_) {
    const auto& themes = loader_->getDiscoveredThemes();
    items.reserve(themes.size() + 1);
    for (int i = 0; i < static_cast<int>(themes.size()); i++) {
      const std::string id = themes[i].id;
      if (sdActive && id == SETTINGS.sdThemeName) current = i + 1;

      ListItem item;
      item.title = themes[i].name;
      item.value = (sdActive && id == SETTINGS.sdThemeName) ? tr(STR_SELECTED) : "";
      item.onSelect = [this, id] {
        SETTINGS.uiTheme = CrossPointSettings::SD_THEME;
        strncpy(SETTINGS.sdThemeName, id.c_str(), sizeof(SETTINGS.sdThemeName) - 1);
        SETTINGS.sdThemeName[sizeof(SETTINGS.sdThemeName) - 1] = '\0';
        finish();
      };
      items.push_back(std::move(item));
    }
  }

  list_.setItems(std::move(items));
  list_.setSelectedIndex(current);
}

void ThemeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    list_.triggerSelected();
    return;
  }

  buttonNavigator_.onNext([this] {
    list_.down();
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this] {
    list_.up();
    requestUpdate();
  });
}

void ThemeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto labels =
    mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const Rect body = UIPage::render(renderer, tr(STR_UI_THEME), nullptr, labels);
  list_.render(renderer, body);
  renderer.displayBuffer();
}
