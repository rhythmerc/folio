#include "CollectionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/CollectionGroupActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/ui/UIPage/UIPage.h"
#include "stores/collections/CollectionStore.h"

namespace {
// "1 book" / "N books" — matches the count meta used across the collection UI.
std::string bookCountLabel(int n) {
  return std::to_string(n) + " " + (n == 1 ? tr(STR_BOOK_SINGULAR) : tr(STR_BOOK_PLURAL));
}
}  // namespace

void CollectionsActivity::buildList() {
  std::vector<ListItem> items;

  // Auto-group rows open a group list; a » chevron marks them as drill-downs.
  auto groupItem = [this](const char* label, uint8_t mode) {
    ListItem item;
    item.title = label;
    item.value = "\xC2\xBB";  // U+00BB (»)
    item.onSelect = [this, mode] {
      startActivityForResult(std::make_unique<CollectionGroupActivity>(renderer, mappedInput, mode),
                             [this](const ActivityResult& res) { onGroupPicked(res); });
    };
    return item;
  };
  items.push_back(groupItem(tr(STR_BY_SERIES), CrossPointSettings::LIB_VIEW_SERIES));
  items.push_back(groupItem(tr(STR_BY_AUTHOR), CrossPointSettings::LIB_VIEW_AUTHOR));
  items.push_back(groupItem(tr(STR_BY_GENRE), CrossPointSettings::LIB_VIEW_GENRE));

  ListItem header;
  header.title = tr(STR_YOUR_COLLECTIONS);
  header.header = true;
  items.push_back(std::move(header));

  ListItem newCollection;
  newCollection.title = tr(STR_NEW_COLLECTION);
  newCollection.bold = true;
  newCollection.onSelect = [this] { promptNewCollection(); };
  items.push_back(std::move(newCollection));

  const bool collectionViewActive = SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_COLLECTION;
  for (const auto& c : COLLECTION_STORE.getCollections()) {
    const bool active = collectionViewActive && SETTINGS.libraryViewCollectionId == c.id;
    const uint32_t id = c.id;

    ListItem item;
    // Mark the active manual collection with a leading checkmark.
    item.title = active ? std::string("\xE2\x9C\x93 ") + c.name : c.name;  // U+2713 (✓)
    item.value = bookCountLabel(c.memberCount());
    item.onSelect = [this, id] {
      SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_COLLECTION;
      SETTINGS.libraryViewCollectionId = id;
      SETTINGS.libraryViewName[0] = '\0';
      SETTINGS.saveToFile();
      finish();  // return to Library; it rebuilds to show the selected collection
    };
    items.push_back(std::move(item));
  }

  list.valueMetaStyle = true;  // smaller italic meta column (count / chevron)
  list.setItems(std::move(items));
}

void CollectionsActivity::promptNewCollection() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NAME_COLLECTION),
                                                                 std::string(), 48, InputType::Text),
                         [this](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(res.data);
                             COLLECTION_STORE.createCollection(kb.text);
                           }
                           buildList();  // reflect the new collection; stay on the list
                         });
}

void CollectionsActivity::onGroupPicked(const ActivityResult& res) {
  // The group list applies its own SETTINGS view change and signals via the
  // result: a real pick (not cancelled) propagates the return all the way to
  // Library; a Back-out leaves us on the collections list.
  if (!res.isCancelled) finish();
}

void CollectionsActivity::onEnter() {
  Activity::onEnter();
  COLLECTION_STORE.loadFromFile();
  buildList();
  requestUpdate();
}

void CollectionsActivity::onExit() { Activity::onExit(); }

void CollectionsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();  // return to the suspended Library (or home if launched standalone)
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    list.triggerSelected();
    return;
  }

  if (list.size() > 0) {
    buttonNavigator.onNext([this] {
      list.down();
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      list.up();
      requestUpdate();
    });
  }
}

void CollectionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const Rect body = UIPage::render(renderer, tr(STR_COLLECTIONS), nullptr, labels);

  list.render(renderer, body);

  renderer.displayBuffer();
}
