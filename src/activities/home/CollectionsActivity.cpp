#include "CollectionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "stores/collections/CollectionStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/home/CollectionGroupActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
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

void CollectionsActivity::buildRows() {
  rows.clear();
  rows.push_back({RowKind::GroupSeries, 0, tr(STR_BY_SERIES), 0, false});
  rows.push_back({RowKind::GroupAuthor, 0, tr(STR_BY_AUTHOR), 0, false});
  rows.push_back({RowKind::GroupGenre, 0, tr(STR_BY_GENRE), 0, false});
  rows.push_back({RowKind::Header, 0, tr(STR_YOUR_COLLECTIONS), 0, false});
  rows.push_back({RowKind::NewCollection, 0, tr(STR_NEW_COLLECTION), 0, false});

  const bool collectionViewActive = SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_COLLECTION;
  for (const auto& c : COLLECTION_STORE.getCollections()) {
    const bool active = collectionViewActive && SETTINGS.libraryViewCollectionId == c.id;
    rows.push_back({RowKind::ManualCollection, c.id, c.name, c.memberCount(), active});
  }
}

bool CollectionsActivity::isSelectable(int index) const {
  return index >= 0 && index < static_cast<int>(rows.size()) && rows[index].kind != RowKind::Header;
}

int CollectionsActivity::nextSelectable(int from) const {
  const int n = static_cast<int>(rows.size());
  if (n == 0) return 0;
  int idx = from;
  for (int step = 0; step < n; ++step) {
    idx = ButtonNavigator::nextIndex(idx, n);
    if (isSelectable(idx)) return idx;
  }
  return from;
}

int CollectionsActivity::prevSelectable(int from) const {
  const int n = static_cast<int>(rows.size());
  if (n == 0) return 0;
  int idx = from;
  for (int step = 0; step < n; ++step) {
    idx = ButtonNavigator::previousIndex(idx, n);
    if (isSelectable(idx)) return idx;
  }
  return from;
}

void CollectionsActivity::onEnter() {
  Activity::onEnter();
  COLLECTION_STORE.loadFromFile();
  buildRows();
  selectedIndex = isSelectable(0) ? 0 : nextSelectable(0);
  requestUpdate();
}

void CollectionsActivity::onExit() { Activity::onExit(); }

void CollectionsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = static_cast<int>(rows.size());
  if (itemCount > 0) {
    buttonNavigator.onNext([this] {
      selectedIndex = nextSelectable(selectedIndex);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      selectedIndex = prevSelectable(selectedIndex);
      requestUpdate();
    });
  }
}

void CollectionsActivity::handleSelection() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(rows.size())) return;
  const Row& row = rows[selectedIndex];

  switch (row.kind) {
    case RowKind::Header:
      return;  // not selectable
    case RowKind::GroupSeries:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_SERIES),
          [](const ActivityResult&) {});
      return;
    case RowKind::GroupAuthor:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_AUTHOR),
          [](const ActivityResult&) {});
      return;
    case RowKind::GroupGenre:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_GENRE),
          [](const ActivityResult&) {});
      return;
    case RowKind::NewCollection:
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NAME_COLLECTION),
                                                                     std::string(), 48, InputType::Text),
                             [this](const ActivityResult& res) {
                               if (!res.isCancelled) {
                                 const auto& kb = std::get<KeyboardResult>(res.data);
                                 COLLECTION_STORE.createCollection(kb.text);
                               }
                               buildRows();
                             });
      return;
    case RowKind::ManualCollection:
      SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_COLLECTION;
      SETTINGS.libraryViewCollectionId = row.collectionId;
      SETTINGS.libraryViewName[0] = '\0';
      SETTINGS.saveToFile();
      activityManager.goHome();
      return;
  }
}

void CollectionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& td = *GUI.getData();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  const auto body = UIPage::render(
      renderer,
      tr(STR_COLLECTIONS),
      nullptr,
      labels
  );

  GUI.drawList(
      renderer, body, static_cast<int>(rows.size()), selectedIndex,
      [this](int index) {
        const Row& r = rows[index];
        // Mark the active manual collection with a leading checkmark.
        if (r.kind == RowKind::ManualCollection && r.active) {
          return std::string("\xE2\x9C\x93 ") + r.label;  // U+2713 (✓)
        }
        return r.label;
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        const Row& r = rows[index];
        switch (r.kind) {
          case RowKind::GroupSeries:
          case RowKind::GroupAuthor:
          case RowKind::GroupGenre:
            return std::string("\xC2\xBB");  // U+00BB (»)
          case RowKind::ManualCollection:
            return bookCountLabel(r.count);
          case RowKind::Header:
          case RowKind::NewCollection:
          default:
            return std::string("");
        }
      },
      false,                                                                     // highlightValue
      nullptr,                                                                   // rowDimmed
      [this](int index) { return rows[index].kind == RowKind::Header; },         // rowIsHeader
      [this](int index) { return rows[index].kind == RowKind::NewCollection; },  // rowBold
      true                                                                       // valueMetaStyle (smaller italic)
  );


  renderer.displayBuffer();
}
