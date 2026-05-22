#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// One indexed book in the library. Persisted to /.crosspoint/library.bin so we
// don't re-parse every EPUB on every entry into LibraryActivity.
//
// Progress is stored as (spineIndex, spineCount). The spine pair is enough to
// recompute the percentage without re-opening the EPUB, and progress.bin
// (written by EpubReaderActivity) is already in this format.
struct LibraryBook {
  std::string path;
  std::string title;
  std::string author;
  // std::hash<std::string>{}(path), matching Epub's cachePath hash so we can
  // locate the per-book cache dir as /.crosspoint/epub_<pathHash>/.
  uint32_t pathHash = 0;
  // 0 = unread; otherwise 1-based spine position from progress.bin.
  uint16_t progressSpineIndex = 0;
  // Total spine entries — needed to compute percentage without re-opening
  // the EPUB. Zero means "unknown" (treat as unread for display purposes).
  uint16_t spineCount = 0;

  bool hasProgress() const { return progressSpineIndex > 0 && spineCount > 0; }

  // 0..100. Returns 0 when unread/unknown.
  uint8_t progressPercent() const {
    if (!hasProgress()) return 0;
    const uint32_t pct = static_cast<uint32_t>(progressSpineIndex) * 100u / spineCount;
    return pct > 100u ? uint8_t{100} : static_cast<uint8_t>(pct);
  }
};

class LibraryIndex {
  static LibraryIndex instance;
  std::vector<LibraryBook> books;
  bool loaded = false;

 public:
  static LibraryIndex& getInstance() { return instance; }

  // Loads /.crosspoint/library.bin if present. Returns true if anything was loaded.
  // Safe to call repeatedly — re-reads from disk every time.
  bool loadFromFile();

  // Walks /Books (recursive, depth-limited), indexes any EPUBs not already in
  // the cache, refreshes progress for known books, and persists library.bin
  // if anything changed. Shows a "Indexing library..." popup if a renderer is
  // supplied. Returns true on success (an empty library is a successful result).
  bool refreshFromSdCard(GfxRenderer* progressRenderer = nullptr);

  const std::vector<LibraryBook>& getBooks() const { return books; }
  int getBookCount() const { return static_cast<int>(books.size()); }
  bool isEmpty() const { return books.empty(); }
  bool isLoaded() const { return loaded; }

  // Pagination helpers (LibraryActivity uses perPage = 9 for the 3×3 grid).
  int totalPages(int perPage) const;
  // Returns nullptr when (page, slot) is past the end of the index.
  const LibraryBook* getAt(int page, int slot, int perPage) const;

  // Re-read progress.bin for a single book and update the in-memory entry.
  // Cheap — opens the 6-byte progress file. Persists library.bin if changed.
  // Called by LibraryActivity after returning from the reader.
  void refreshProgress(const std::string& path);

  // Drop the in-memory books vector and release its storage. The on-disk
  // /.crosspoint/library.bin is preserved. The next loadFromFile() call
  // repopulates the index. Used by LibraryActivity::onExit() to release
  // ~25 KB of heap to whatever activity comes next.
  void unload();

 private:
  LibraryIndex() = default;
  bool saveToFile() const;
};

#define LIBRARY_INDEX LibraryIndex::getInstance()
