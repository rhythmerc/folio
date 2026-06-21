#pragma once

#include <cstdint>
#include <vector>

#include "stores/util/JsonStore.h"

// One book's reading position, keyed by the shared path hash (see util/PathHash.h).
// Fields mirror the legacy per-book progress.bin (spineIndex, pageNumber,
// pageCount) so this store is a drop-in superset for both the reader (resume)
// and LibraryIndex (shelf percentage).
struct BookProgress {
  uint32_t pathHash = 0;
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
};

// On-disk payload: field name is the JSON key (PFR-reflected).
struct ProgressFile {
  std::vector<BookProgress> entries;
};

// Library-wide reading progress, persisted to /.crosspoint/progress.json. One
// file read serves the whole library shelf, replacing the old per-book
// progress.bin open during indexing. Lookup is a linear scan: trivial against
// the 500-book cap and dwarfed by the SD I/O it removed.
class ProgressStore : public JsonStore<ProgressFile> {
  static ProgressStore instance;
  ProgressStore() : JsonStore("/.crosspoint/progress.json", "PRG") {}

 public:
  static ProgressStore& getInstance() { return instance; }

  // Reading position for a book, or nullptr if none stored.
  const BookProgress* find(uint32_t pathHash) const;

  // Upsert the book's position and persist. No-op (no write) when unchanged,
  // honoring the SD/SPIFFS write-throttling rule.
  void put(uint32_t pathHash, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount);
};

#define PROGRESS_STORE ProgressStore::getInstance()
