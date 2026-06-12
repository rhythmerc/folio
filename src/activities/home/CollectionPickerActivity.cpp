#include "CollectionPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/ui/UIPage/UIPage.h"
#include "fontIds.h"
#include "stores/collections/CollectionStore.h"

namespace {
// "1 book" / "N books" — matches the count meta used across the collection UI.
std::string bookCountLabel(int n) {
  return std::to_string(n) + " " + (n == 1 ? tr(STR_BOOK_SINGULAR) : tr(STR_BOOK_PLURAL));
}
}  // namespace

void CollectionPickerActivity::buildList() {
  std::vector<ListItem> items;
  const auto& collections = COLLECTION_STORE.getCollections();
  items.reserve(collections.size() + 1);

  for (const auto& c : collections) {
    const bool member = c.hasBook(bookHash);
    if (mode == Mode::Remove && !member) continue;

    const uint32_t id = c.id;
    ListItem item;
    item.value = bookCountLabel(c.memberCount());
    if (mode == Mode::Add) {
      // Mark collections the book is already in with a leading checkmark.
      item.title = member ? std::string("\xE2\x9C\x93 ") + c.name : c.name;  // U+2713 (✓)
      item.onSelect = [this, id] {
        COLLECTION_STORE.addBookToCollection(bookHash, id);
        finish();
      };
    } else {
      item.title = c.name;
      item.onSelect = [this, id] {
        COLLECTION_STORE.removeBookFromCollection(bookHash, id);
        finish();
      };
    }
    items.push_back(std::move(item));
  }

  if (mode == Mode::Add) {
    ListItem newCollection;
    newCollection.title = tr(STR_NEW_COLLECTION);
    newCollection.bold = true;
    newCollection.onSelect = [this] { promptNewCollection(); };
    items.push_back(std::move(newCollection));
  }

  empty_ = items.empty();  // only reachable in Remove mode
  list.valueMetaStyle = true;
  list.setItems(std::move(items));
}

void CollectionPickerActivity::promptNewCollection() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NAME_COLLECTION),
                                                                 std::string(), 48, InputType::Text),
                         [this](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(res.data);
                             const uint32_t id = COLLECTION_STORE.createCollection(kb.text);
                             if (id != 0) {
                               COLLECTION_STORE.addBookToCollection(bookHash, id);
                               finish();
                               return;
                             }
                           }
                           // Cancelled or create failed: refresh and stay on the picker.
                           buildList();
                         });
}

void CollectionPickerActivity::onEnter() {
  Activity::onEnter();
  COLLECTION_STORE.loadFromFile();
  buildList();
  requestUpdate();
}

void CollectionPickerActivity::onExit() { Activity::onExit(); }

void CollectionPickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
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

void CollectionPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const char* title = (mode == Mode::Add) ? tr(STR_ADD_TO_COLLECTION) : tr(STR_REMOVE_FROM_COLLECTION);

  const Rect body = UIPage::render(renderer, title, nullptr, labels);

  if (empty_) {
    const int y = body.y + (body.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_NOT_IN_ANY_COLLECTION));
  } else {
    list.render(renderer, body);
  }

  renderer.displayBuffer();
}
