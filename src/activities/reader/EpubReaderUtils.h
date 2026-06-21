#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include "stores/progress/ProgressStore.h"
#include "util/PathHash.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  FsFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }

  // Mirror into the library-wide store (the fast path the shelf reads). The
  // per-book progress.bin above stays as a backup for opt-in recovery.
  PROGRESS_STORE.put(hashPath(epub.getPath()), static_cast<uint16_t>(spineIndex),
                     static_cast<uint16_t>(pageNumber), static_cast<uint16_t>(pageCount));

  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
