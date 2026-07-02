#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>
#include <util/BookmarkUtil.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderFontSystem.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "UiThemeLoader.h"
#include "components/UITheme.h"
#include "stores/progress/ProgressStore.h"
#include "util/PathHash.h"
#include "components/icons/arrow-bear-right40.h"
#include "components/icons/autoturn40.h"
#include "components/icons/bookmark-filled40.h"
#include "components/icons/bookmark40.h"
#include "components/icons/footnote40.h"
#include "components/icons/more40.h"
#include "components/icons/rotate40.h"
#include "components/icons/tools40.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr float bookmarkProgressEpsilon = 0.0001f;

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  // Resume position: prefer the library-wide store; fall back to the legacy
  // per-book progress.bin so a book opened for the first time after upgrade
  // keeps its place (the next saveProgress backfills the store via dual-write).
  if (const BookProgress* prog = PROGRESS_STORE.find(hashPath(epub->getPath()))) {
    currentSpineIndex = prog->spineIndex;
    nextPageNumber = prog->pageNumber;
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = prog->pageCount;
    LOG_DBG("ERS", "Loaded progress (store): %d, %d", currentSpineIndex, nextPageNumber);
  } else {
    FsFile f;
    if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
      uint8_t data[6];
      int dataSize = f.read(data, 6);
      if (dataSize == 4 || dataSize == 6) {
        currentSpineIndex = data[0] + (data[1] << 8);
        nextPageNumber = data[2] + (data[3] << 8);
        cachedSpineIndex = currentSpineIndex;
        LOG_DBG("ERS", "Loaded progress (legacy bin): %d, %d", currentSpineIndex, nextPageNumber);
      }
      if (dataSize == 6) {
        cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      }
    }
  }

  if (nextPageNumber == UINT16_MAX) {
    // UINT16_MAX is an in-memory navigation sentinel for "open previous chapter
    // on its last page". It should never be treated as persisted resume state
    // after sleep or reopen.
    LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
    nextPageNumber = 0;
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Load this book's bookmarks so the status-bar indicator and toggle icon are correct on open.
  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Leaving the reader: unload the eager SD font family to reclaim its resident
  // tables (rebinds on next reader entry via ReaderActivity::ensureLoaded).
  readerFontSystem.releaseFonts(renderer);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Confirm long-press toggles a bookmark when SETTINGS.longPressMenuFunction is set to it.
  // (longPressButtonBehavior below is the separate SIDE-button hold; this is the Confirm hold.)
  // Back — not Confirm — opens the GlobalMenu, so Confirm is otherwise free in the reader.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (SETTINGS.longPressMenuFunction == CrossPointSettings::LP_MENU_BOOKMARK && !bookmarkLongPressFired &&
        mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
      addBookmark();  // fires requestUpdate(); status-bar indicator refreshes on the redraw
      bookmarkLongPressFired = true;
    }
  } else {
    bookmarkLongPressFired = false;  // re-arm once Confirm is released
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    // Auto-turn is turned off from the sidebar's Auto-turn submenu, not by a button
    // press: Back opens the GlobalMenu and Confirm is unbound in the reader.
    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Back ownership: while viewing a footnote the sidebar is disabled
  // (getGlobalMenuConfig gates on footnoteDepth == 0), so Back returns from the
  // footnote here. At top level Back belongs to the GlobalMenu — it is consumed in
  // ActivityManager to open/close the sidebar, and Library/File Browser are reached
  // via the menu's Apps entry. The reader must NOT act on Back at top level: the
  // release following a menu-close press would otherwise fall through and exit the book.
  // if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
  //   restoreSavedPosition();
  //   return;
  // }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::selectChapter() {
  const int spineIdx = currentSpineIndex;
  const std::string path = epub->getPath();
  startActivityForResult(
      std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
          RenderLock lock(*this);
          currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
          nextPageNumber = 0;
          section.reset();
        }
      });
}

void EpubReaderActivity::goToPercent() {
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             jumpToPercent(std::get<PercentResult>(result.data).percent);
                           }
                         });
}

void EpubReaderActivity::openBookmarks() {
  const std::string path = epub->getPath();
  startActivityForResult(
      std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, path),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& jump = std::get<ProgressChangeResult>(result.data);
          if (currentSpineIndex != jump.spineIndex || (section && section->currentPage != jump.page)) {
            RenderLock lock(*this);
            currentSpineIndex = jump.spineIndex;
            nextPageNumber = jump.page;
            section.reset();
          }
        }
        loadCachedBookmarks();
      });
}

void EpubReaderActivity::displayQr() {
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    auto p = section->loadPageFromSectionFile();
    if (p) {
      std::string fullText;
      for (const auto& el : p->elements) {
        if (el->getTag() == TAG_PageLine) {
          const auto& line = static_cast<const PageLine&>(*el);
          if (line.getBlock()) {
            const auto& words = line.getBlock()->getWords();
            for (const auto& w : words) {
              if (!fullText.empty()) fullText += " ";
              fullText += w;
            }
          }
        }
      }
      if (!fullText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                               [this](const ActivityResult& result) {});
        return;
      }
    }
  }
  // If no text or page loading failed, just close menu
  requestUpdate();
}

void EpubReaderActivity::takeScreenshot() {
  {
    RenderLock lock(*this);
    pendingScreenshot = true;
  }
  requestUpdate();
}

void EpubReaderActivity::deleteCacheAndExit() {
  {
    RenderLock lock(*this);
    if (epub && section) {
      uint16_t backupSpine = currentSpineIndex;
      uint16_t backupPage = section->currentPage;
      uint16_t backupPageCount = section->pageCount;
      section.reset();
      epub->clearCache();
      epub->setupCacheDir();
      if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
        LOG_ERR("ERS", "Failed to save progress before cache clear");
      }
    }
  }
  onGoHome();
}

void EpubReaderActivity::startSync() {
  if (!KOREADER_STORE.hasCredentials()) {
    return;
  }
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return;
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
}

std::vector<PopupMenuEntry> EpubReaderActivity::navigateItems() {
  std::vector<PopupMenuEntry> items{
      PopupMenuEntry{.label = tr(STR_SELECT_CHAPTER), .onSelected = [this]() { selectChapter(); return true; }},
      PopupMenuEntry{.label = tr(STR_GO_TO_PERCENT), .onSelected = [this]() { goToPercent(); return true; }},
  };

  if (!cachedBookmarks.empty()) {
    items.push_back(
        PopupMenuEntry{.label = tr(STR_BOOKMARKS), .onSelected = [this]() { openBookmarks(); return true; }});
  }

  return items;
}

std::vector<PopupMenuEntry> EpubReaderActivity::orientationItems() {
  static constexpr StrId labels[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                     StrId::STR_LANDSCAPE_CCW};
  std::vector<PopupMenuEntry> items;
  items.reserve(std::size(labels));
  for (uint8_t i = 0; i < std::size(labels); ++i) {
    items.push_back(PopupMenuEntry{
        .label = I18N.get(labels[i]),
        .glyph = SETTINGS.orientation == i ? std::optional<PopupMenu::Glyph>(PopupMenu::Glyph::Circle) : std::nullopt,
        .onSelected = [this, i]() { applyOrientation(i); return true; }});
  }
  return items;
}

std::vector<PopupMenuEntry> EpubReaderActivity::autoTurnItems() {
  // Index into PAGE_TURN_RATES; 0 = Off. Labels mirror the former reader menu.
  const char* labels[] = {tr(STR_STATE_OFF), "1", "3", "6", "12"};
  std::vector<PopupMenuEntry> items;
  items.reserve(std::size(labels));
  for (uint8_t i = 0; i < std::size(labels); ++i) {
    items.push_back(PopupMenuEntry{
        .label = labels[i],
        .glyph = autoTurnOption == i ? std::optional<PopupMenu::Glyph>(PopupMenu::Glyph::Circle) : std::nullopt,
        .onSelected = [this, i]() { toggleAutoPageTurn(i); return true; }});
  }
  return items;
}

std::vector<PopupMenuEntry> EpubReaderActivity::toolItems() {
  std::vector<PopupMenuEntry> items{
      PopupMenuEntry{.label = tr(STR_SCREENSHOT_BUTTON), .onSelected = [this]() { takeScreenshot(); return true; }},
      PopupMenuEntry{.label = tr(STR_DISPLAY_QR), .onSelected = [this]() { displayQr(); return true; }},
  };
  // Sync only appears when KOReader credentials are configured; keep Delete last.
  if (KOREADER_STORE.hasCredentials()) {
    items.push_back(
        PopupMenuEntry{.label = tr(STR_SYNC_PROGRESS), .onSelected = [this]() { startSync(); return true; }});
  }
  items.push_back(
      PopupMenuEntry{.label = tr(STR_DELETE_CACHE), .onSelected = [this]() { deleteCacheAndExit(); return true; }});
  return items;
}

std::vector<PopupMenuEntry> EpubReaderActivity::footnoteItems() {
  std::vector<PopupMenuEntry> items;
  items.reserve(
      currentPageFootnotes.size() +
      footnoteDepth > 0 ? 1 : 0
  );

  if(footnoteDepth > 0) {
    items.push_back(PopupMenuEntry{
        .label = tr(STR_BACK_TO_PREVIOUS),
        .onSelected = [this]() {
          restoreSavedPosition();
          return true;
        }
    });
  }

  for (const auto& footnote : currentPageFootnotes) {
    std::string href = footnote.href;
    // PopupMenuEntry::label is a non-owning const char*. Point it at the member
    // vector's own char[] (stable for the menu's lifetime) or the static tr()
    // fallback — never a local std::string, which dangles after this iteration.
    const char* label = footnote.number[0] ? footnote.number : tr(STR_LINK);

    items.push_back(PopupMenuEntry{
        .label = label,
        .onSelected = [this, href]() {
          navigateToHref(href, true);
          return true;
        }
    });
  }

  return items;
}

std::vector<MenuRegistryEntry> EpubReaderActivity::buildTallMenuEntries() {
  std::vector<MenuRegistryEntry> entries{
      MenuRegistryEntry{
        .icon = {40, 40, ArrowBearRight40Icon},
        .name = tr(STR_NAVIGATE),
        .popupItems = navigateItems()
      },
      MenuRegistryEntry{
        // keepOpenOnPress: toggling rebuilds entries so the icon flips live.
        .icon = currentPageBookmarked ? MenuRegistryEntry::Icon{40, 40, BookmarkFilled40Icon}
                                      : MenuRegistryEntry::Icon{40, 40, Bookmark40Icon},
        .name = tr(STR_TOGGLE_BOOKMARK),
        .onPress = [this]() { addBookmark(); },
        .keepOpenOnPress = true
      },
      MenuRegistryEntry{
        .icon = {40, 40, Rotate40Icon},
        .name = tr(STR_ORIENTATION),
        .popupItems = orientationItems()
      },
      MenuRegistryEntry{
        .icon = {40, 40, Autoturn40Icon},
        .name = tr(STR_AUTO_TURN_PAGES_PER_MIN),
        .popupItems = autoTurnItems()
      },
      MenuRegistryEntry{
        .icon = {40, 40, Tools40Icon},
        .name = tr(STR_TOOLS),
        .popupItems = toolItems()
      },
  };

  auto footnotes = footnoteItems();

  // Footnotes belongs after Navigate + Bookmark, but only when the page has any.
  if (!footnotes.empty()) {
    entries.insert(
      entries.begin() + 2,
      MenuRegistryEntry{
        .icon = {40, 40, Footnote40Icon},
        .name = tr(STR_FOOTNOTES),
        .popupItems = footnotes
      });
  }
  return entries;
}

std::vector<MenuRegistryEntry> EpubReaderActivity::buildWideMenuEntries() {
  // Landscape is only ~480px tall, so fold everything past the two most-used
  // actions under a single "More" entry to stay within the nav strip's slots.
  std::vector<PopupMenuEntry> more{
      PopupMenuEntry{
        .label = tr(STR_ORIENTATION),
        .initialSelectedChild = SETTINGS.orientation,
        .children = orientationItems()
      },
      PopupMenuEntry{
        .label = tr(STR_AUTO_TURN_PAGES_PER_MIN),
        .initialSelectedChild = autoTurnOption,
        .children = autoTurnItems()
      },
  };

  auto footnotes = footnoteItems();
  if (!footnotes.empty()) {
    more.insert(
      more.begin(), 
      PopupMenuEntry{
        .label = tr(STR_FOOTNOTES),
        .children = footnoteItems()
      }
    );
  }

  auto tools = toolItems();
  more.reserve(more.size() + tools.size());
  more.insert(more.end(), tools.begin(), tools.end());

  return std::vector<MenuRegistryEntry>{
      MenuRegistryEntry{
        .icon = {40, 40, ArrowBearRight40Icon},
        .name = tr(STR_NAVIGATE),
        .popupItems = navigateItems()
      },
      MenuRegistryEntry{
        // keepOpenOnPress: toggling rebuilds entries so the icon flips live.
        .icon = currentPageBookmarked ? MenuRegistryEntry::Icon{40, 40, BookmarkFilled40Icon}
                                      : MenuRegistryEntry::Icon{40, 40, Bookmark40Icon},
        .name = tr(STR_TOGGLE_BOOKMARK),
        .onPress = [this]() { addBookmark(); },
        .keepOpenOnPress = true
      },
      MenuRegistryEntry{
        .icon = {40, 40, More40Icon},
        .name = tr(STR_MORE),
        .popupItems = std::move(more)
      }
  };
}

std::vector<MenuRegistryEntry> EpubReaderActivity::getGlobalMenuEntries() {
  // Tall (portrait/inverted) orientations have room for every action; wide
  // (landscape) orientations fold the extras under "More".
  return renderer.getScreenHeight() > renderer.getScreenWidth() ? buildTallMenuEntries() : buildWideMenuEntries();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  autoTurnOption = selectedPageTurnOption;
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + GUI.getData()->statusBar.verticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    // New section = new layout; drop the page cache so we re-read from SD.
    cachedPage.reset();
    cachedPageNumber = -1;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.fontSize,
                                      [](uint8_t pt) { return SETTINGS.readerFontIdForPointSize(pt); }, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Refresh the bookmark indicator for the page about to be drawn.
  updateBookmarkFlag();

  {
    // Re-read from SD only on a cache miss. The same page re-renders many times
    // while the GlobalMenu overlay is open; the content is identical, so the
    // ~100ms SD read + deserialize is pure waste on those repeats.
    if (!cachedPage || cachedPageNumber != section->currentPage) {
      const auto tSdStart = millis();
      cachedPage = section->loadPageFromSectionFile();
      LOG_DBG("ERS", "loadPageFromSectionFile: %lums", millis() - tSdStart);
      if (!cachedPage) {
        cachedPageNumber = -1;
        automaticPageTurnActive = false;
        if (pageLoadRetries == 0) {
          // First failure is most likely a corrupt/truncated section cache.
          // Clear it and rebuild once; the next render retries the load.
          LOG_ERR("ERS", "Failed to load page from SD - clearing section cache and rebuilding");
          pageLoadRetries++;
          section->clearCache();
          section.reset();
          requestUpdate();
        } else {
          // Rebuild didn't help — the source is bad. Stop retrying (no
          // requestUpdate => no infinite loop) and show a terminal error; the
          // user presses Back to leave, handled in loop().
          // ponytail: single shared counter, reset only on success — a second
          // distinct corrupt page won't get its own rebuild, which is fine.
          LOG_ERR("ERS", "Page load still failing after rebuild - giving up");
          renderer.clearScreen();
          renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_ERROR_GENERAL_FAILURE), true, EpdFontFamily::BOLD);
          renderer.displayBuffer();
        }
        showPendingSyncSaveError();
        return;
      }
      pageLoadRetries = 0;
      cachedPageNumber = section->currentPage;

      // Collect footnotes from the freshly loaded page. The footnote menu labels
      // point into currentPageFootnotes, so refresh only when the page actually
      // changed, or the label pointers dangle mid-menu. Moving footnotes out of
      // the cached page is safe: they are never read from the page again.
      if (currentSpineIndex != footnotesSpineIndex || section->currentPage != footnotesPage) {
        currentPageFootnotes = std::move(cachedPage->footnotes);
        footnotesSpineIndex = currentSpineIndex;
        footnotesPage = section->currentPage;
      }
    }

    const auto start = millis();
    renderContents(*cachedPage, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  // Only persist when the position actually changed; render() re-runs on every
  // button press while the global menu is open, and an unguarded SD write there
  // dominated re-render latency (and burns SPIFFS erase cycles).
  if (currentSpineIndex != savedSpineIndex || section->currentPage != savedPageNumber ||
      section->pageCount != savedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
      savedSpineIndex = currentSpineIndex;
      savedPageNumber = section->currentPage;
      savedPageCount = section->pageCount;
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.fontSize,
                                     [](uint8_t pt) { return SETTINGS.readerFontIdForPointSize(pt); })) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop, const int orientedMarginRight,
                                        const int orientedMarginBottom, const int orientedMarginLeft) {
  const auto t0 = millis();

  // Self-warming: no prewarm scan pass — the real render below warms the
  // SdCardFont overflow + kern-row caches on demand (flash fonts self-warm via
  // the FontDecompressor group-LRU). Experiment: measure page-turn latency
  // against the old prewarm path; tPrewarm stays as a (now ~0) marker.
  const auto tPrewarm = millis();

  const int fontId = SETTINGS.getReaderFontId();
  const bool pageHasImages = page.hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  // Grayscale pass renders the whole page when text AA is on, otherwise only the
  // images — so an image page with AA off still gets 16-level images.
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page.render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page.renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  // Fast render mode now only concerns IMAGES: text is 1-bit (no grayscale to
  // dither), so it renders straight to the BW framebuffer in the normal pass.
  // Threshold text, then blank the image box and blue-noise dither the images
  // into the single BW framebuffer and flush once — no storeBwBuffer, no
  // LSB/MSB grayscale planes, no grayscale waveform. The Quality path below is
  // untouched.
  if (SETTINGS.grayscaleRenderMode == CrossPointSettings::GR_FAST && pageHasImages) {
    renderer.setBwImageCacheEnabled(bwImageCacheActive);
    // Threshold text first; blank the image box so threshold ink from the main
    // pass doesn't bleed through, then dither images only.
    page.render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.setDitherBw(true);
      page.renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.setDitherBw(false);
    }
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    LOG_DBG("ERS", "Page render (fast dither, images only): prewarm=%lums total=%lums", tPrewarm - t0, millis() - t0);
    return;
  }

  // Under the GlobalMenu (BW re-renders), let images keep a RAM BW copy so each
  // menu keypress doesn't re-stream the image from SD. No-op while reading
  // (flag false); grayscale passes never see it (menu-open returns before them).
  renderer.setBwImageCacheEnabled(bwImageCacheActive);
  page.render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust, so
    // blank only the image area and do two fast refreshes. This is also the fast
    // BW image path used when the nav menu is open (grayscale skipped below).
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render to restore images into the blanked area. Status bar is not
      // re-rendered to avoid reading stale dynamic values (e.g. battery %).
      page.render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The grayscale pass below leaves gray charge in the image region that a
    // plain fast diff on the next page can't clear (text would ghost gray), so
    // force the next page onto the HALF ghost-cleanup path.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // While the GlobalMenu overlays the reader, skip the grayscale pass: it is far
  // too slow under the overlay and its waveform fights the menu's BW compositing.
  // The BW frame just drawn (incl. the image double-blank above) is what shows.
  if (activityManager.globalMenu.isOpen()) {
    // ponytail: page cache kills the SD read on these repeats; the ~70ms BW
    // re-render stays. Caching that too needs the 48KB framebuffer resident,
    // which this 380KB MCU can't spare — leave it.
    LOG_DBG("ERS", "Page render (menu open): prewarm=%lums bw_render=%lums display=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tDisplay - t0);
    return;
  }

  // Tiled grayscale: render each plane band-by-band into a small scratch streamed
  // straight to the controller, leaving the BW framebuffer intact (no ~48KB
  // storeBwBuffer). The page is re-rendered ceil(H/STRIP_ROWS) times per plane,
  // but renderCharImpl culls out-of-band glyphs before decode so the cost stays
  // close to one render. Both text and images honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tEnd - tGrayDisplay, tEnd - t0);
    }
  } else if (needsAnyGrayscale) {
    // Fallback for a controller without strip support (unreachable on current
    // hardware — all drivers support strip). Save/restore the BW frame around
    // the full-buffer grayscale passes.
    if (!renderer.storeBwBuffer()) {
      LOG_ERR("ERS", "Failed to store BW buffer for grayscale; skipping AA this page");
      return;
    }
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderGrayscalePass();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderGrayscalePass();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += GUI.getData()->statusBar.verticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const ProgressRange pageRange =
      getPageProgressRange(epub, currentSpineIndex, section->currentPage, section->pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, section->pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
