#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "stores/util/JsonStore.h"

// A user-created manual collection: a named set of books, referenced by the
// same path hash the rest of the cache uses (std::hash<path>) so moving a file
// keeps it in its collections. Persisted to /.crosspoint/collections.json.
// Membership is edited from the Library popup (Book → Add/Remove from Collection).
struct Collection {
  uint32_t id = 0;
  std::string name;
  std::vector<uint32_t> members;  // path hashes of collection members

  int memberCount() const { return static_cast<int>(members.size()); }

  // True when the book (by path hash) is a member of this collection.
  bool hasBook(uint32_t bookHash) const {
    return std::find(members.begin(), members.end(), bookHash) != members.end();
  }
};

// On-disk payload: field names are the JSON keys (PFR-reflected).
struct CollectionsFile {
  std::vector<Collection> collections;
};

class CollectionStore : public JsonStore<CollectionsFile> {
  static CollectionStore instance;
  uint32_t nextId = 1;  // derived (not persisted): one past the highest id seen

  CollectionStore() : JsonStore("/.crosspoint/collections.json", "COL") {}

 protected:
  void onLoaded() override;

 public:
  static CollectionStore& getInstance() { return instance; }

  const std::vector<Collection>& getCollections() const { return data_.collections; }
  bool isEmpty() const { return data_.collections.empty(); }

  // Look up a collection by id. Returns nullptr when not found.
  const Collection* findById(uint32_t id) const;

  // Create a new (empty) collection with the given name and persist. Returns
  // the new collection id, or 0 on failure (e.g. empty name).
  uint32_t createCollection(const std::string& name);

  // Remove a collection by id and persist. No-op if the id is unknown.
  void removeCollection(uint32_t id);

  void addBookToCollection(uint32_t bookHash, uint32_t collectionId);
  void removeBookFromCollection(uint32_t bookHash, uint32_t collectionId);
};

#define COLLECTION_STORE CollectionStore::getInstance()
