#include "LibraryIndex.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string_view>
#include <unordered_map>

#include "components/UITheme.h"

namespace {
constexpr char LOG_TAG[] = "LIB";

constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char BOOKS_ROOT[] = "/Books";
constexpr char LIBRARY_FILE[] = "/.crosspoint/library.bin";
constexpr char LIBRARY_FILE_TMP[] = "/.crosspoint/library.bin.tmp";

// 'XPLB' (CrossPoint Library) in little-endian byte order on the wire.
constexpr uint32_t LIBRARY_FILE_MAGIC = 0x424C5058u;
constexpr uint8_t LIBRARY_FILE_VERSION = 1;

// Bound directory recursion. /Books/Author/Series/Title.epub is plenty;
// deeper hierarchies are uncommon in book libraries and recursing further
// risks exhausting SdFat's open-file pool.
constexpr int MAX_TRAVERSAL_DEPTH = 4;

// Generated alongside metadata so LibraryActivity can render covers from
// /.crosspoint/epub_<hash>/thumb_144.bmp directly. Matches the prototype's
// 96×144 cover size.
constexpr int THUMB_HEIGHT = 144;

// Soft cap: ~500 × ~240 bytes per LibraryBook ≈ 120 KB. Beyond this we
// abort the scan rather than exhaust the heap. The reader is a focused
// tool, not a library manager.
constexpr int MAX_LIBRARY_BOOKS = 500;

uint32_t hashPath(const std::string& path) {
  return static_cast<uint32_t>(std::hash<std::string>{}(path));
}

std::string cachePathFor(uint32_t pathHash) {
  return std::string(CACHE_DIR) + "/epub_" + std::to_string(pathHash);
}

// Reads progress.bin into spineIdx (1-based spine position). Leaves spineIdx
// at 0 if no progress file exists or it's too short — i.e., "unread".
void readProgress(const std::string& bookCachePath, uint16_t& spineIdx) {
  spineIdx = 0;
  FsFile f;
  const std::string progressPath = bookCachePath + "/progress.bin";
  if (!Storage.openFileForRead(LOG_TAG, progressPath.c_str(), f)) {
    return;
  }
  uint8_t data[2];
  const int n = f.read(data, sizeof(data));
  if (n == sizeof(data)) {
    spineIdx = static_cast<uint16_t>(data[0] | (data[1] << 8));
  }
}

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
    serialization::readString(f, b.path);
    serialization::readString(f, b.title);
    serialization::readString(f, b.author);
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
    serialization::writeString(f, b.path);
    serialization::writeString(f, b.title);
    serialization::writeString(f, b.author);
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
      uint16_t oldProgress = b.progressSpineIndex;
      readProgress(cachePathFor(h), b.progressSpineIndex);
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
    b.spineCount = static_cast<uint16_t>(epub.getSpineItemsCount());
    readProgress(epub.getCachePath(), b.progressSpineIndex);

    // Best-effort thumbnail at 144px tall — failures here just mean we'll
    // render a title-only fallback tile in LibraryActivity.
    epub.generateThumbBmp(THUMB_HEIGHT);

    books.push_back(std::move(b));
    newBooksIndexed++;
    changed = true;
  }

  // Any books left in existingByHash were removed from disk between runs.
  if (!existingByHash.empty()) {
    changed = true;
  }

  // Stable display order: alphabetic by title (falling back to author then path).
  std::sort(books.begin(), books.end(), [](const LibraryBook& a, const LibraryBook& b) {
    if (a.title != b.title) return a.title < b.title;
    if (a.author != b.author) return a.author < b.author;
    return a.path < b.path;
  });

  loaded = true;

  if (changed) {
    saveToFile();
  }

  LOG_DBG(LOG_TAG, "Library refresh: %d books indexed (%d new)", static_cast<int>(books.size()),
          newBooksIndexed);
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
  if (idx >= books.size()) return nullptr;
  return &books[idx];
}

void LibraryIndex::refreshProgress(const std::string& path) {
  const uint32_t h = hashPath(path);
  auto it = std::find_if(books.begin(), books.end(),
                         [h, &path](const LibraryBook& b) { return b.pathHash == h && b.path == path; });
  if (it == books.end()) return;

  const uint16_t old = it->progressSpineIndex;
  readProgress(cachePathFor(h), it->progressSpineIndex);
  if (it->progressSpineIndex != old) {
    saveToFile();
  }
}
