#include "CollectionStore.h"

#include <Logging.h>

#include <algorithm>

namespace {
constexpr char LOG_TAG[] = "COL";
}  // namespace

CollectionStore CollectionStore::instance;

void CollectionStore::onLoaded() {
  // nextId is derived state: one past the highest collection id seen on disk.
  nextId = 1;
  for (const auto& c : data_.collections) {
    if (c.id >= nextId) {
      nextId = c.id + 1;
    }
  }
  LOG_DBG(LOG_TAG, "collections.json loaded: %d collection(s)", static_cast<int>(data_.collections.size()));
}

const Collection* CollectionStore::findById(uint32_t id) const {
  for (const auto& c : data_.collections) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

uint32_t CollectionStore::createCollection(const std::string& name) {
  if (name.empty()) {
    return 0;
  }

  Collection c;
  c.id = nextId++;
  c.name = name;
  const uint32_t newId = c.id;
  data_.collections.push_back(std::move(c));

  if (!saveToFile()) {
    // Roll back the in-memory add so state matches disk.
    data_.collections.pop_back();
    nextId = newId;
    return 0;
  }
  return newId;
}

void CollectionStore::removeCollection(uint32_t id) {
  auto& collections = data_.collections;

  int removed = std::erase_if(collections, [id](const Collection& c) { return c.id == id; });

  if (removed != 0) {
    saveToFile();
  }
}

void CollectionStore::addBookToCollection(uint32_t bookHash, uint32_t collectionId) {
  auto collectionIt = std::find_if(data_.collections.begin(), data_.collections.end(), [collectionId](const Collection& c) { return c.id == collectionId; });
  if (collectionIt == data_.collections.end()) {
    // collection not found
    return;
  }
  auto& collection = *collectionIt;

  if (collection.hasBook(bookHash)) {
    // book is already in collection; nothing to do
    return;
  }

  collection.members.push_back(bookHash);
  saveToFile();
}

void CollectionStore::removeBookFromCollection(uint32_t bookHash, uint32_t collectionId) {
  auto collectionIt = std::find_if(data_.collections.begin(), data_.collections.end(), [collectionId](const Collection& c) { return c.id == collectionId; });
  if (collectionIt == data_.collections.end()) {
    // collection not found
    return;
  }
  auto& collection = *collectionIt;

  const auto removed = std::erase(collection.members, bookHash);
  if (removed != 0) {
    saveToFile();
  }
}
