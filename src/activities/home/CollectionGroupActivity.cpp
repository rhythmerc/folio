#include "CollectionGroupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/ui/UIPage/UIPage.h"
#include "fontIds.h"
#include "util/Flex.h"

namespace {
// "1 book" / "N books" — matches the prototype's right-aligned row-meta count.
std::string bookCountLabel(int n) {
  return std::to_string(n) + " " + (n == 1 ? tr(STR_BOOK_SINGULAR) : tr(STR_BOOK_PLURAL));
}
}  // namespace

const char* CollectionGroupActivity::headerTitle() const {
  switch (mode) {
    case CrossPointSettings::LIB_VIEW_SERIES:
      return tr(STR_BY_SERIES);
    case CrossPointSettings::LIB_VIEW_AUTHOR:
      return tr(STR_BY_AUTHOR);
    case CrossPointSettings::LIB_VIEW_GENRE:
      return tr(STR_BY_GENRE);
  }
  return tr(STR_COLLECTIONS);
}

void CollectionGroupActivity::buildGroups() {
  groups.clear();

  std::map<std::string, int> counts;  // ordered by name, dedupes
  const int count = LIBRARY_INDEX.getBookCount();
  for (int i = 0; i < count; ++i) {
    const BookView b = LIBRARY_INDEX.getAt(i);
    // Genre holds multiple newline-joined subjects: count the book once per
    // subject so it lists under every one of its genres.
    if (mode == CrossPointSettings::LIB_VIEW_GENRE) {
      forEachGenre(b.genre, [&counts](std::string_view g) { counts[std::string(g)]++; });
      continue;
    }
    std::string_view field;
    switch (mode) {
      case CrossPointSettings::LIB_VIEW_SERIES:
        field = b.series;
        break;
      case CrossPointSettings::LIB_VIEW_AUTHOR:
        field = b.author;
        break;
    }
    if (field.empty()) continue;  // skip books without this field
    counts[std::string(field)]++;
  }

  groups.reserve(counts.size());
  for (auto& kv : counts) {
    groups.emplace_back(kv.first, kv.second);
  }
}

void CollectionGroupActivity::onEnter() {
  Activity::onEnter();
  if (!LIBRARY_INDEX.isLoaded()) {
    LIBRARY_INDEX.loadFromFile();
  }
  buildGroups();
  selectedIndex = 0;
  requestUpdate();
}

void CollectionGroupActivity::onExit() { Activity::onExit(); }

void CollectionGroupActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Signal "no group chosen" so the parent collections list stays put.
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(groups.size())) {
      SETTINGS.libraryViewKind = mode;
      SETTINGS.libraryViewCollectionId = 0;
      strncpy(SETTINGS.libraryViewName, groups[selectedIndex].first.c_str(), sizeof(SETTINGS.libraryViewName) - 1);
      SETTINGS.libraryViewName[sizeof(SETTINGS.libraryViewName) - 1] = '\0';
      SETTINGS.saveToFile();
      // Default (not-cancelled) result tells the parent a group was chosen, so
      // the return propagates through CollectionsActivity back to the Library.
      finish();
    }
    return;
  }

  const int itemCount = static_cast<int>(groups.size());
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void CollectionGroupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& td = *GUI.getData();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  const auto body = UIPage::render(
      renderer,
      headerTitle(),
      nullptr,
      labels
  );

  if (groups.empty()) {
    const int y = body.y + (body.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  } else {
    GUI.drawList(
        renderer, body, static_cast<int>(groups.size()), selectedIndex,
        [this](int index) { return groups[index].first; }, nullptr, nullptr,
        [this](int index) { return bookCountLabel(groups[index].second); }, false, nullptr, nullptr, nullptr,
        /*valueMetaStyle=*/true);
  }

  renderer.displayBuffer();
}
