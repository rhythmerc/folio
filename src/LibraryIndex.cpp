#include "LibraryIndex.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "components/UITheme.h"
#include "stores/progress/ProgressStore.h"
#include "util/PathHash.h"

namespace {
constexpr char LOG_TAG[] = "LIB";

constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char BOOKS_ROOT[] = "/Books";
constexpr char LIBRARY_FILE[] = "/.crosspoint/library.bin";
constexpr char LIBRARY_FILE_TMP[] = "/.crosspoint/library.bin.tmp";

// 'XPLB' (CrossPoint Library) in little-endian byte order on the wire.
constexpr uint32_t LIBRARY_FILE_MAGIC = 0x424C5058u;
// v2 adds LibraryBook::openSequence. The load path's version-mismatch branch
// triggers a full disk rescan, so old caches regenerate transparently with
// openSequence = 0 on every book.
// v3 adds series/genre/seriesIndex (auto-group collections).
constexpr uint8_t LIBRARY_FILE_VERSION = 5;

// Bound directory recursion. /Books/Author/Series/Title.epub is plenty;
// deeper hierarchies are uncommon in book libraries and recursing further
// risks exhausting SdFat's open-file pool.
constexpr int MAX_TRAVERSAL_DEPTH = 4;

// Thumb height — see LibraryIndex::THUMB_HEIGHT declaration in the header
// for the rationale (matches COVER_H to skip draw-time downscaling).
constexpr int THUMB_HEIGHT = LibraryIndex::THUMB_HEIGHT;

// Soft cap: ~500 × ~240 bytes per LibraryBook ≈ 120 KB. Beyond this we
// abort the scan rather than exhaust the heap. The reader is a focused
// tool, not a library manager.
constexpr int MAX_LIBRARY_BOOKS = 500;

struct DirFrame {
  HalFile dir;
  std::string path;
};

}  // namespace

LibraryIndex LibraryIndex::instance;

bool LibraryIndex::loadFromFile() {
  loaded = false;
  books.clear();

  if (!Storage.exists(LIBRARY_FILE)) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(LOG_TAG, LIBRARY_FILE, f)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t reserved[3] = {0, 0, 0};
  uint32_t bookCount = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, reserved);
  serialization::readPod(f, bookCount);

  if (magic != LIBRARY_FILE_MAGIC) {
    LOG_ERR(LOG_TAG, "library.bin: bad magic 0x%08X", magic);
    return false;
  }
  if (version != LIBRARY_FILE_VERSION) {
    LOG_DBG(LOG_TAG, "library.bin: version %u, will rebuild", version);
    return false;
  }
  if (bookCount > static_cast<uint32_t>(MAX_LIBRARY_BOOKS)) {
    LOG_ERR(LOG_TAG, "library.bin: bookCount %u exceeds cap %d", bookCount, MAX_LIBRARY_BOOKS);
    return false;
  }

  books.reserve(bookCount);
  for (uint32_t i = 0; i < bookCount; ++i) {
    LibraryBook b;
    serialization::readPod(f, b.pathHash);
    serialization::readPod(f, b.progressSpineIndex);
    serialization::readPod(f, b.spineCount);
    serialization::readPod(f, b.openSequence);
    serialization::readString(f, b.path);
    serialization::readString(f, b.title);
    serialization::readString(f, b.author);
    serialization::readString(f, b.series);
    serialization::readString(f, b.genre);
    serialization::readPod(f, b.seriesIndex);
    books.push_back(std::move(b));
  }

  loaded = true;
  LOG_DBG(LOG_TAG, "library.bin loaded: %d books", static_cast<int>(books.size()));
  return true;
}

bool LibraryIndex::saveToFile() const {
  Storage.mkdir(CACHE_DIR);

  FsFile f;
  if (!Storage.openFileForWrite(LOG_TAG, LIBRARY_FILE_TMP, f)) {
    LOG_ERR(LOG_TAG, "Cannot open %s for write", LIBRARY_FILE_TMP);
    return false;
  }

  const uint32_t magic = LIBRARY_FILE_MAGIC;
  const uint8_t version = LIBRARY_FILE_VERSION;
  const uint8_t reserved[3] = {0, 0, 0};
  const uint32_t bookCount = static_cast<uint32_t>(books.size());
  serialization::writePod(f, magic);
  serialization::writePod(f, version);
  serialization::writePod(f, reserved);
  serialization::writePod(f, bookCount);

  for (const auto& b : books) {
    serialization::writePod(f, b.pathHash);
    serialization::writePod(f, b.progressSpineIndex);
    serialization::writePod(f, b.spineCount);
    serialization::writePod(f, b.openSequence);
    serialization::writeString(f, b.path);
    serialization::writeString(f, b.title);
    serialization::writeString(f, b.author);
    serialization::writeString(f, b.series);
    serialization::writeString(f, b.genre);
    serialization::writePod(f, b.seriesIndex);
  }

  // Must close before rename — see CLAUDE.md DESTRUCTOR_CLOSES_FILE note.
  f.close();

  if (Storage.exists(LIBRARY_FILE)) {
    Storage.remove(LIBRARY_FILE);
  }
  if (!Storage.rename(LIBRARY_FILE_TMP, LIBRARY_FILE)) {
    LOG_ERR(LOG_TAG, "Failed to rename %s -> %s", LIBRARY_FILE_TMP, LIBRARY_FILE);
    return false;
  }
  return true;
}

bool LibraryIndex::refreshFromSdCard(GfxRenderer* progressRenderer) {
  if (!Storage.exists(BOOKS_ROOT)) {
    LOG_DBG(LOG_TAG, "%s not present — empty library", BOOKS_ROOT);
    const bool changed = !books.empty();
    books.clear();
    loaded = true;
    if (changed) {
      saveToFile();
    }
    return true;
  }

  // Move the currently-loaded books into a hash-keyed lookup so we can reuse
  // metadata for already-indexed EPUBs without re-parsing them.
  std::unordered_map<uint32_t, LibraryBook> existingByHash;
  existingByHash.reserve(books.size() + 16);
  for (auto& b : books) {
    const uint32_t h = b.pathHash;
    existingByHash.emplace(h, std::move(b));
  }
  const size_t previousCount = existingByHash.size();
  books.clear();
  books.reserve(previousCount + 16);

  HalFile root = Storage.open(BOOKS_ROOT);
  if (!root || !root.isDirectory()) {
    LOG_ERR(LOG_TAG, "%s is not a directory", BOOKS_ROOT);
    return false;
  }
  root.rewindDirectory();

  std::vector<DirFrame> stack;
  stack.reserve(MAX_TRAVERSAL_DEPTH);
  stack.push_back({std::move(root), BOOKS_ROOT});

  Rect popupRect;
  bool popupShown = false;
  int newBooksIndexed = 0;
  bool changed = false;

  while (!stack.empty()) {
    DirFrame& frame = stack.back();
    HalFile entry = frame.dir.openNextFile();
    if (!entry) {
      frame.dir.close();
      stack.pop_back();
      continue;
    }

    char nameBuf[256];
    entry.getName(nameBuf, sizeof(nameBuf));

    // Skip hidden files and the Windows-created sentinel directory.
    if (nameBuf[0] == '.' || std::strcmp(nameBuf, "System Volume Information") == 0) {
      continue;
    }

    if (entry.isDirectory()) {
      if (stack.size() < static_cast<size_t>(MAX_TRAVERSAL_DEPTH)) {
        std::string subpath = frame.path + "/" + nameBuf;
        entry.rewindDirectory();
        stack.push_back({std::move(entry), std::move(subpath)});
      }
      continue;
    }

    if (!FsHelpers::hasEpubExtension(std::string_view{nameBuf})) {
      continue;
    }

    if (books.size() >= static_cast<size_t>(MAX_LIBRARY_BOOKS)) {
      LOG_ERR(LOG_TAG, "Hit MAX_LIBRARY_BOOKS=%d; skipping remaining EPUBs", MAX_LIBRARY_BOOKS);
      break;
    }

    std::string path = frame.path + "/" + nameBuf;
    const uint32_t h = hashPath(path);

    // Reuse cached metadata if the path matches — guards against the unlikely
    // case of two different paths hashing to the same value.
    auto it = existingByHash.find(h);
    if (it != existingByHash.end() && it->second.path == path) {
      LibraryBook b = std::move(it->second);
      existingByHash.erase(it);
      const uint16_t oldProgress = b.progressSpineIndex;
      const BookProgress* prog = PROGRESS_STORE.find(h);
      b.progressSpineIndex = prog ? prog->spineIndex : 0;
      if (b.progressSpineIndex != oldProgress) {
        changed = true;
      }
      books.push_back(std::move(b));
      continue;
    }

    // New book — show the popup the first time we have actual work to do.
    if (progressRenderer && !popupShown) {
      popupRect = GUI.drawPopup(*progressRenderer, tr(STR_LIBRARY_INDEXING));
      popupShown = true;
    }

    Epub epub(path, CACHE_DIR);
    // buildIfMissing=true so we get a populated book.bin metadata cache;
    // skipLoadingCss=true because we only need title/author/spine info.
    if (!epub.load(true, true)) {
      LOG_ERR(LOG_TAG, "Epub load failed: %s", path.c_str());
      continue;
    }

    LibraryBook b;
    b.path = std::move(path);
    b.pathHash = h;
    b.title = epub.getTitle();
    b.author = epub.getAuthor();
    b.series = epub.getSeries();
    b.genre = epub.getGenre();
    b.seriesIndex = epub.getSeriesIndex();
    b.spineCount = static_cast<uint16_t>(epub.getSpineItemsCount());
    const BookProgress* prog = PROGRESS_STORE.find(h);
    b.progressSpineIndex = prog ? prog->spineIndex : 0;

    // Best-effort thumbnail. Failures here just mean we'll render a
    // title-only fallback tile in LibraryActivity.
    epub.generateThumbBmp(LibraryIndex::THUMB_MAX_WIDTH, THUMB_HEIGHT);

    books.push_back(std::move(b));
    newBooksIndexed++;
    changed = true;
  }

  // Any books left in existingByHash were removed from disk between runs.
  if (!existingByHash.empty()) {
    changed = true;
  }

  // Sort order is owned by LibraryActivity now (it reads CrossPointSettings
  // and calls sortBy()). Leave books in scan order here.

  loaded = true;

  if (changed) {
    saveToFile();
  }

  LOG_DBG(LOG_TAG, "Library refresh: %d books indexed (%d new)", static_cast<int>(books.size()), newBooksIndexed);
  return true;
}

int LibraryIndex::totalPages(int perPage) const {
  if (perPage <= 0) return 0;
  if (books.empty()) return 0;
  return (static_cast<int>(books.size()) + perPage - 1) / perPage;
}

const LibraryBook* LibraryIndex::getAt(int page, int slot, int perPage) const {
  if (page < 0 || slot < 0 || perPage <= 0 || slot >= perPage) return nullptr;
  const size_t idx = static_cast<size_t>(page) * static_cast<size_t>(perPage) + static_cast<size_t>(slot);

  return this->getAt(idx);
}

const LibraryBook* LibraryIndex::getAt(int index) const {
  if (index >= books.size()) return nullptr;
  return &books[index];
}

void LibraryIndex::unload() {
  books.clear();
  books.shrink_to_fit();
  loaded = false;
}

namespace {
// Case-insensitive (ASCII fold) substring test.
bool containsCI(std::string_view haystack, std::string_view needle) {
  const auto eq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), eq) != haystack.end();
}
}  // namespace

std::vector<const LibraryBook*> LibraryIndex::search(std::string_view query) const {
  // Trim surrounding whitespace; a blank query matches nothing (not everything).
  const auto first = query.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = query.find_last_not_of(" \t");
  query = query.substr(first, last - first + 1);

  std::vector<const LibraryBook*> matches;
  for (const auto& b : books) {
    // genre is the raw newline-joined <dc:subject> string; a plain substring
    // test over it is sufficient (queries don't span the '\n' separator).
    if (containsCI(b.title, query) || containsCI(b.author, query) || containsCI(b.series, query) ||
        containsCI(b.genre, query)) {
      matches.push_back(&b);
    }
  }
  return matches;
}

void LibraryIndex::refreshProgress(const std::string& path) {
  const uint32_t h = hashPath(path);
  auto it = std::find_if(books.begin(), books.end(),
                         [h, &path](const LibraryBook& b) { return b.pathHash == h && b.path == path; });
  if (it == books.end()) return;

  const uint16_t old = it->progressSpineIndex;
  const BookProgress* prog = PROGRESS_STORE.find(h);
  it->progressSpineIndex = prog ? prog->spineIndex : 0;
  if (it->progressSpineIndex != old) {
    saveToFile();
  }
}

void LibraryIndex::noteBookOpened(const std::string& path) {
  const uint32_t h = hashPath(path);
  auto it = std::find_if(books.begin(), books.end(),
                         [h, &path](const LibraryBook& b) { return b.pathHash == h && b.path == path; });
  if (it == books.end()) return;  // Not indexed (e.g. opened from file browser).

  uint32_t maxSeq = 0;
  for (const auto& b : books) {
    if (b.openSequence > maxSeq) maxSeq = b.openSequence;
  }
  it->openSequence = maxSeq + 1;
  saveToFile();
}

namespace {

// Strip a single leading article ("The ", "A ", "An ") from a title for sort
// purposes. ASCII-only — matches the prototype's normalisation.
std::string_view titleSortKey(const std::string& title) {
  std::string_view v{title};
  auto startsWithCi = [&](std::string_view prefix) -> bool {
    if (v.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
      char a = v[i];
      char b = prefix[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != b) return false;
    }
    return true;
  };
  if (startsWithCi("the "))
    v.remove_prefix(4);
  else if (startsWithCi("an "))
    v.remove_prefix(3);
  else if (startsWithCi("a "))
    v.remove_prefix(2);
  return v;
}

// The last whitespace-separated token of `author` (the surname for typical
// "First Last" inputs). Returns the full string when there's no whitespace.
std::string_view authorSurname(const std::string& author) {
  std::string_view v{author};
  const size_t sp = v.find_last_of(" \t");
  if (sp == std::string_view::npos) return v;
  return v.substr(sp + 1);
}

int ciCompare(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return (ca < cb) ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return (a.size() < b.size()) ? -1 : 1;
}

}  // namespace

void LibraryIndex::sortBy(SortField field, SortDirection direction) {
  const bool asc = (direction == SortDirection::Ascending);

  // Comparator returns "a comes first" relative to the *natural* direction
  // for the field, then we flip at the end if descending was requested.
  // Books with no usable key (unread / no author / never opened) always
  // sort to the END, regardless of direction — matches the prototype.
  std::sort(books.begin(), books.end(), [&](const LibraryBook& a, const LibraryBook& b) {
    auto tiebreak = [&]() {
      const int t = ciCompare(titleSortKey(a.title), titleSortKey(b.title));
      if (t != 0) return t < 0;
      return a.path < b.path;
    };

    switch (field) {
      case SortField::Recent: {
        const bool aHas = a.hasBeenOpened();
        const bool bHas = b.hasBeenOpened();
        if (aHas != bHas) return aHas;  // opened books before never-opened
        if (!aHas) return tiebreak();
        if (a.openSequence != b.openSequence) {
          return asc ? (a.openSequence < b.openSequence) : (a.openSequence > b.openSequence);
        }
        return tiebreak();
      }
      case SortField::Title: {
        const int c = ciCompare(titleSortKey(a.title), titleSortKey(b.title));
        if (c != 0) return asc ? (c < 0) : (c > 0);
        return a.path < b.path;
      }
      case SortField::Author: {
        const bool aHas = !a.author.empty();
        const bool bHas = !b.author.empty();
        if (aHas != bHas) return aHas;
        if (!aHas) return tiebreak();
        const int c = ciCompare(authorSurname(a.author), authorSurname(b.author));
        if (c != 0) return asc ? (c < 0) : (c > 0);
        const int c2 = ciCompare(a.author, b.author);
        if (c2 != 0) return asc ? (c2 < 0) : (c2 > 0);
        return tiebreak();
      }
      case SortField::Progress: {
        const bool aHas = a.hasProgress();
        const bool bHas = b.hasProgress();
        if (aHas != bHas) return aHas;
        if (!aHas) return tiebreak();
        const uint8_t pa = a.progressPercent();
        const uint8_t pb = b.progressPercent();
        if (pa != pb) return asc ? (pa < pb) : (pa > pb);
        return tiebreak();
      }
    }
    return tiebreak();
  });
}
