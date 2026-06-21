#include "ProgressStore.h"

#include <Logging.h>

ProgressStore ProgressStore::instance;

const BookProgress* ProgressStore::find(uint32_t pathHash) const {
  for (const auto& e : data_.entries) {
    if (e.pathHash == pathHash) return &e;
  }
  return nullptr;
}

void ProgressStore::put(uint32_t pathHash, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount) {
  for (auto& e : data_.entries) {
    if (e.pathHash != pathHash) continue;
    if (e.spineIndex == spineIndex && e.pageNumber == pageNumber && e.pageCount == pageCount) {
      return;  // unchanged — skip the rewrite
    }
    e.spineIndex = spineIndex;
    e.pageNumber = pageNumber;
    e.pageCount = pageCount;
    saveToFile();
    return;
  }

  data_.entries.push_back(BookProgress{
      .pathHash = pathHash,
      .spineIndex = spineIndex,
      .pageNumber = pageNumber,
      .pageCount = pageCount,
  });
  saveToFile();
}
