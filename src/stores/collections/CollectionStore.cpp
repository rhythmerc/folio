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
  const auto it =
      std::remove_if(collections.begin(), collections.end(), [id](const Collection& c) { return c.id == id; });
  if (it == collections.end()) {
    return;  // unknown id, nothing removed
  }
  collections.erase(it, collections.end());
  saveToFile();
}
