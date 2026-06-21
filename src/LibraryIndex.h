#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class GfxRenderer;

// A book's genre is stored as its EPUB <dc:subject> tags joined by '\n'.
// Invokes fn(std::string_view) once per non-empty subject. Templated on the
// callback to avoid std::function heap/binary overhead (see CLAUDE.md).
template <typename Fn>
inline void forEachGenre(const std::string& genre, Fn&& fn) {
  size_t start = 0;
  while (start < genre.size()) {
    size_t nl = genre.find('\n', start);
    if (nl == std::string::npos) nl = genre.size();
    if (nl > start) fn(std::string_view(genre).substr(start, nl - start));
    start = nl + 1;
  }
}

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
  // Series/genre drive the "By Series" / "By Genre" auto-group collections.
  // Parsed from the EPUB OPF and cached so grouping needs no re-parse.
  std::string series;
  std::string genre;
  // 0 = no/unknown series index. Cached for future Details/series sort.
  uint16_t seriesIndex = 0;
  // std::hash<std::string>{}(path), matching Epub's cachePath hash so we can
  // locate the per-book cache dir as /.crosspoint/epub_<pathHash>/.
  uint32_t pathHash = 0;
  // 0 = unread; otherwise 1-based spine position from progress.bin.
  uint16_t progressSpineIndex = 0;
  // Total spine entries — needed to compute percentage without re-opening
  // the EPUB. Zero means "unknown" (treat as unread for display purposes).
  uint16_t spineCount = 0;
  // Monotonic open counter, bumped by noteBookOpened() each time the reader
  // launches this book. 0 = never opened. Used by the Recently Opened sort.
  // No RTC on the device, so a counter is cheaper than an absolute timestamp.
  uint32_t openSequence = 0;

  bool hasProgress() const { return progressSpineIndex > 0 && spineCount > 0; }
  bool hasBeenOpened() const { return openSequence > 0; }

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
  // Max dimensions (in pixels) of the cover thumbnail generated for each book
  // during indexing. The converter preserves aspect ratio and shrinks to fit
  // — actual on-disk dims may be smaller along one axis.
  //
  // THUMB_HEIGHT matches LibraryActivity's COVER_H, so the on-disk thumb is
  // already the target draw height — no down-scaling at paint time, and the
  // pre-rasterized 1-bit cache survives orientation changes (cover-area
  // width varies between portrait and landscape, but height is fixed).
  //
  // THUMB_MAX_WIDTH is sized to fit within the narrower portrait cellW
  // (~129 px) with a few pixels of visual breathing room. Wider source
  // covers (square, graphic-novel aspect) get to use most of the cell
  // instead of being clamped to a fixed 0.6-aspect frame; taller covers are
  // unaffected since height binds first.
  static constexpr int THUMB_HEIGHT = 120;
  static constexpr int THUMB_MAX_WIDTH = 120;

  // Sort options for the library shelf. Plain enums so this header has no
  // dependency on CrossPointSettings. LibraryActivity maps from settings.
  enum class SortField : uint8_t {
    Recent = 0,    // by openSequence (unread sinks to the end)
    Title = 1,     // alphabetic, leading articles ignored
    Author = 2,    // surname first, empty author sinks to the end
    Progress = 3,  // by progressPercent, unread sinks to the end
  };
  enum class SortDirection : uint8_t {
    Descending = 0,
    Ascending = 1,
  };

  static LibraryIndex& getInstance() { return instance; }

  // Loads /.crosspoint/library.bin if present. Returns true if anything was loaded.
  // Safe to call repeatedly — re-reads from disk every time.
  bool loadFromFile();

  // Walks /Books (recursive, depth-limited), indexes any EPUBs not already in
  // the cache, refreshes progress for known books, and persists library.bin
  // if anything changed. Shows a "Indexing library..." popup if a renderer is
  // supplied. Returns true on success (an empty library is a successful result).
  bool refreshFromSdCard(GfxRenderer* progressRenderer = nullptr);

  // Books whose metadata contains `query` as a case-insensitive substring.
  // Searches all available text metadata: title, author, series, and genre.
  // A blank query returns an empty result. Returned pointers index into the
  // books vector and are valid only until the next sortBy()/load (same
  // contract as getBooks()).
  std::vector<const LibraryBook*> search(std::string_view query) const;

  const std::vector<LibraryBook>& getBooks() const { return books; }
  int getBookCount() const { return static_cast<int>(books.size()); }
  bool isEmpty() const { return books.empty(); }
  bool isLoaded() const { return loaded; }

  // Pagination helpers (LibraryActivity uses perPage = 9 for the 3×3 grid).
  int totalPages(int perPage) const;
  // Returns nullptr when (page, slot) is past the end of the index.
  const LibraryBook* getAt(int page, int slot, int perPage) const;
  const LibraryBook* getAt(int index) const;

  // Refresh a single book's progress from ProgressStore (updated by the reader)
  // and update the in-memory entry. No SD read — the store is already resident.
  // Persists library.bin if changed. Called by LibraryActivity after returning
  // from the reader.
  void refreshProgress(const std::string& path);

  // Bump the matching book's openSequence to (max openSequence across the
  // index) + 1 and persist library.bin. No-op when the path isn't indexed
  // (e.g. a book opened directly from the file browser). Cheap: one O(N)
  // scan + one library.bin rewrite. Called only when the reader is
  // actually entered, not on page turns.
  void noteBookOpened(const std::string& path);

  // Reorder `books` in place per the requested field and direction. Pure
  // function of the books vector + settings inputs — no Settings coupling.
  // LibraryActivity calls this on entry and after popup-menu changes.
  void sortBy(SortField field, SortDirection direction);

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
