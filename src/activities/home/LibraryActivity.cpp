#include "LibraryActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/RenderLock.h"
#include "activities/home/CollectionPickerActivity.h"
#include "activities/home/CollectionsActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/bookalt40.h"
#include "components/icons/books40.h"
#include "components/icons/search40.h"
#include "components/icons/sort40.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.generated.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"
#include "components/ui/Cover/Cover.h"
#include "components/ui/ProgressBar/ProgressBar.h"
#include "components/ui/TextBlock/TextBlock.h"
#include "components/ui/UIPage/UIPage.h"
#include "stores/collections/CollectionStore.h"
#include "util/Flex.h"
#include "util/GridHelper.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

int libFont(FontRole role) { return GUI.getFontForRole(role); }

constexpr int HEADER_HEIGHT = 89;
constexpr int HEADER_BOTTOM_BORDER = 3;
constexpr int FOOTER_HEIGHT = 40;

constexpr int CONTENT_PAD_X = 18;
constexpr int CONTENT_PAD_Y = 8;
constexpr int RAIL_WIDTH = 18;
constexpr int RAIL_GAP = 10;
constexpr int CELL_GAP = 14;

constexpr int kTilePadTop = 4;
constexpr int kTilePadBottom = 16;
constexpr int kCoverPercent = 60;
constexpr int kCoverBottomPadding = 8;
constexpr int kTitleAuthorGap = 1;

constexpr int kSelectionInsetX = 3;
constexpr int kSelectionLiftAbove = 4;
constexpr int kSelectionExtendBelowTileContent = 8;

// Frame hugs measured content: it floats from kSelectionLiftAbove above the
// cell top down to kSelectionExtendBelowTileContent past `contentBottom` (the
// progress-bar bottom for progress books, the author/title bottom otherwise).
inline Rect computeSelectionFrame(const Rect& cell, int contentBottom) {
  const int top = cell.y - kSelectionLiftAbove;
  return Rect{
      cell.x - kSelectionInsetX,
      top,
      cell.width + kSelectionInsetX * 2,
      (contentBottom + kSelectionExtendBelowTileContent) - top,
  };
}

// Page rail tick geometry.
constexpr int RAIL_PAD_TOP = 8;
static constexpr StrId kSortLabels[] = {StrId::STR_SORT_RECENT, StrId::STR_SORT_TITLE, StrId::STR_SORT_AUTHOR,
                                            StrId::STR_SORT_PROGRESS};
}  // namespace



// ---- Lifecycle --------------------------------------------------------------

void LibraryActivity::onEnter() {
  Activity::onEnter();

  LIBRARY_INDEX.loadFromFile();

  // New EPUBs trigger a blocking "Indexing library..."
  // popup; the popup never appears when nothing has changed.
  LIBRARY_INDEX.refreshFromSdCard(&renderer);

  // Manual-collection membership store, for the COLLECTION view filter.
  COLLECTION_STORE.loadFromFile();

  // Apply persisted sort.
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                       static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));

  // Resolve the persisted active view and build the filtered, ordered list.
  // Safe to do before the prefetch worker exists (no lock needed yet).
  rebuildView();

  switch (SETTINGS.libraryViewKind) {
    case CrossPointSettings::LIB_VIEW_ALL: {
      gridHelper = GridHelper(viewItemCount(), ROWS, COLS, 0);
      break;
    }
    default: {
      // non-ALL views insert a virtual back button as the first
      // entry in the grid. Set index to 1 so we default to the first actual book.
      gridHelper = GridHelper(viewItemCount(), ROWS, COLS, 1);
      break;
    }
  }

  lastObservedPage_ = gridHelper.currentPage();

  // Seed the cover draw envelope (the worker scales each cover to fit it) and
  // the reused decode scratch BEFORE the worker starts. A null scratch is
  // tolerated — decodeThumbInto fails gracefully and the cover shows a
  // placeholder.
  const Rect coverSlot = computeCoverSlot();
  coverBoxW_ = coverSlot.width;
  coverBoxH_ = coverSlot.height;
  decodeScratch_ = makeUniqueNoThrow<uint8_t[]>(DECODE_SCRATCH_BYTES);
  if (!decodeScratch_) LOG_ERR(LOG_TAG, "OOM: cover decode scratch (%u bytes)",
                               static_cast<unsigned>(DECODE_SCRATCH_BYTES));

  // Spin up the prefetch worker before requesting the first paint so it's
  // ready by the time we ask for page fills.
  cacheLock_ = xSemaphoreCreateBinary();
  prefetchSignal_ = xSemaphoreCreateBinary();
  prefetchExited_ = xSemaphoreCreateBinary();
  prefetchBatchDone_ = xSemaphoreCreateBinary();
  assert(cacheLock_ != nullptr && prefetchSignal_ != nullptr && prefetchExited_ != nullptr &&
         prefetchBatchDone_ != nullptr);
  // Binary semaphores from xSemaphoreCreateBinary are born "taken"
  // (count = 0). Give once so the first acquirer can take it.
  xSemaphoreGive(cacheLock_);
  prefetchCancel_ = false;
  prefetchShutdown_ = false;
  prefetchQueueCount_ = 0;
  for (auto& slot : prefetchQueue_) slot = 0xFF;
  xTaskCreate(&prefetchTaskTrampoline, "LibPrefetch",
              4096,     // stack: SD I/O + bitmap parsing
              this, 1,  // priority: same tier as render
              &prefetchTask_);
  assert(prefetchTask_ != nullptr);

  // Swap the cache to the current page and paint immediately: placeholders show
  // first, then the worker's batch-done repaints with real covers (loop()). The
  // render path never decodes — the worker owns all decode + scaling.
  loadPage(lastObservedPage_);
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();

  if (prefetchTask_ != nullptr) {
    xSemaphoreTake(cacheLock_, portMAX_DELAY);

    prefetchShutdown_ = true;
    prefetchCancel_ = true;
    prefetchQueueCount_ = 0;

    xSemaphoreGive(cacheLock_);
    xSemaphoreGive(prefetchSignal_);
    xSemaphoreTake(prefetchExited_, portMAX_DELAY);

    vTaskDelete(prefetchTask_);
    prefetchTask_ = nullptr;
  }

  if (prefetchSignal_ != nullptr) {
    vSemaphoreDelete(prefetchSignal_);
    prefetchSignal_ = nullptr;
  }

  if (prefetchExited_ != nullptr) {
    vSemaphoreDelete(prefetchExited_);
    prefetchExited_ = nullptr;
  }

  if (prefetchBatchDone_ != nullptr) {
    vSemaphoreDelete(prefetchBatchDone_);
    prefetchBatchDone_ = nullptr;
  }

  pageCache_.clear();

  // Worker has joined (vTaskDelete above), so the scratch has no remaining
  // reader and can be freed.
  decodeScratch_.reset();

  if (cacheLock_ != nullptr) {
    vSemaphoreDelete(cacheLock_);
    cacheLock_ = nullptr;
  }

  LIBRARY_INDEX.unload();
  // Note: the global font glyph/group caches are reset centrally by
  // ActivityManager on full navigation (Replace), not here — see
  // ActivityManager::loop().
}

// ---- Prefetch worker --------------------------------------------------------

bool LibraryActivity::buildThumbPath(uint8_t page, uint8_t slot, char* out, std::size_t outSize) const {
  if (out == nullptr || outSize < 64) return false;
  const int idx = static_cast<int>(page) * PER_PAGE + static_cast<int>(slot);
  // bookForGridIndex returns nullptr for the back-tile slot (no cover) and for
  // positions past the end of the filtered view.
  const LibraryBook* book = bookForGridIndex(idx);
  if (book == nullptr) {
    out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "/.crosspoint/epub_%lu/thumb_%d.bmp", static_cast<unsigned long>(book->pathHash),
           LibraryIndex::THUMB_HEIGHT);
  return true;
}

bool LibraryActivity::enqueuePrefetchLocked(uint8_t page) {
  const uint8_t pages = gridHelper.pageCount();
  if (pages == 0 || page >= pages) return false;

  for (uint8_t i = 0; i < prefetchQueueCount_; ++i) {
    if (prefetchQueue_[i] == page) return true;  // already queued
  }

  if (prefetchQueueCount_ >= PREFETCH_QUEUE_CAPACITY) {
    // Drop the oldest pending — newer requests reflect more recent
    // user intent. Shift down one slot.
    for (uint8_t i = 1; i < prefetchQueueCount_; ++i) {
      prefetchQueue_[i - 1] = prefetchQueue_[i];
    }
    --prefetchQueueCount_;
  }

  prefetchQueue_[prefetchQueueCount_++] = page;
  xSemaphoreGive(prefetchSignal_);
  return true;
}

void LibraryActivity::cancelAllPrefetch() {
  if (cacheLock_ == nullptr) return;
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  prefetchCancel_ = true;
  prefetchQueueCount_ = 0;
  for (auto& slot : prefetchQueue_) slot = 0xFF;

  xSemaphoreGive(cacheLock_);
  xSemaphoreGive(prefetchSignal_);
}

void LibraryActivity::loadPage(uint8_t page) {
  if (cacheLock_ == nullptr) return;

  // Cancel any in-flight decode, swap the single-page working set to `page`, and
  // arm the batch-done repaint. The caller paints immediately afterward — the
  // cleared cache peek-misses into placeholders, then the worker's batch-done
  // (loop()) repaints with the now-resident real covers.
  cancelAllPrefetch();

  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  pageCache_.clear();
  pendingPageRender_ = enqueuePrefetchLocked(page);  // false → empty page, no batch-done to await
  xSemaphoreTake(prefetchBatchDone_, 0);             // drop any stale completion signal
  lastObservedPage_ = page;
  xSemaphoreGive(cacheLock_);
}

void LibraryActivity::prefetchTaskTrampoline(void* ctx) {
  auto* self = static_cast<LibraryActivity*>(ctx);
  self->prefetchTaskLoop();

  xSemaphoreGive(self->prefetchExited_);
  while (true) vTaskDelay(portMAX_DELAY);
}

void LibraryActivity::prefetchTaskLoop() {
  while (true) {
    // Wait for work. Activity gives the semaphore on enqueue or on
    // cancel/shutdown (so we always wake to re-check our state).
    xSemaphoreTake(prefetchSignal_, portMAX_DELAY);

    while (true) {
      uint8_t targetPage = 0xFF;
      xSemaphoreTake(cacheLock_, portMAX_DELAY);
      if (prefetchShutdown_) {
        xSemaphoreGive(cacheLock_);
        return;
      }
      if (prefetchQueueCount_ > 0) {
        targetPage = prefetchQueue_[0];
        for (uint8_t i = 1; i < prefetchQueueCount_; ++i) {
          prefetchQueue_[i - 1] = prefetchQueue_[i];
        }
        prefetchQueue_[--prefetchQueueCount_] = 0xFF;
        // Reset the cancel flag for the new work unit. Old in-flight
        // cancellations were already honored by the previous iteration.
        prefetchCancel_ = false;
      }
      xSemaphoreGive(cacheLock_);
      if (targetPage == 0xFF) break;  // queue empty, go back to sleep

      // Decode each of the 9 covers. Path build, cache check, scaling, and
      // cache.set are lock-protected; only the SD decode runs unlocked so the
      // render thread can still touch the cache for hits while we're working.
      bool batchCompleted = true;  // false if we broke early (cancel/shutdown)
      for (uint8_t slot = 0; slot < PER_PAGE; ++slot) {
        if (prefetchCancel_ || prefetchShutdown_) {
          batchCompleted = false;
          break;
        }
        char path[64];
        bool pathOk = false;
        xSemaphoreTake(cacheLock_, portMAX_DELAY);
        pathOk = buildThumbPath(targetPage, slot, path, sizeof(path));
        bool alreadyCached = false;
        if (pathOk) {
          alreadyCached = (pageCache_.get(path) != nullptr);
        }
        xSemaphoreGive(cacheLock_);
        if (!pathOk || alreadyCached) continue;

        // Slow path — decode the 2bpp source into the reused scratch off-lock.
        // HalStorage serializes its own SD access, so the render thread can
        // still read the cache for hits while this runs. The scratch is
        // single-producer (only this worker touches it).
        int srcW = 0;
        int srcH = 0;
        bool topDown = false;
        if (!GfxRenderer::decodeThumbInto(path, decodeScratch_.get(), DECODE_SCRATCH_BYTES, srcW, srcH, topDown)) {
          continue;  // decode failed / no scratch — nothing to insert
        }

        int targetW = 0;
        int targetH = 0;
        GfxRenderer::computeThumbTarget(srcW, srcH, coverBoxW_, coverBoxH_, targetW, targetH);
        if (targetW <= 0 || targetH <= 0) continue;

        if (prefetchCancel_ || prefetchShutdown_) {
          batchCompleted = false;
          break;
        }

        // Scale into a fresh 1bpp arena raster and store, under the lock. The
        // scaling reads the scratch (no other writer) and the arena mutates
        // only here / in eviction — all serialized by cacheLock_.
        xSemaphoreTake(cacheLock_, portMAX_DELAY);
        if (!prefetchShutdown_) {
          BitmapCacheManager::Entry entry;
          entry.path = path;
          entry.width = srcW;
          entry.height = srcH;
          entry.topDown = topDown;
          if (renderer.buildScaledFromSource(entry, decodeScratch_.get(), srcW, srcH, topDown, targetW, targetH,
                                             pageCache_.arena())) {
            pageCache_.set(std::move(entry));
          }
          // else: arena full — skip; the cover paints a placeholder.
        }
        xSemaphoreGive(cacheLock_);
      }
      // Signal the activity loop that this page's covers are resident
      // in the cache. Activity uses this to clear pendingPageRender_
      // and trigger the deferred repaint when the awaited page lands.
      // Suppressed on cancel — the partial state shouldn't paint.
      if (batchCompleted && !prefetchShutdown_) {
        xSemaphoreGive(prefetchBatchDone_);
      }
    }
  }
}

bool LibraryActivity::openCollectionPicker(bool add) {
  const LibraryBook* book = bookForGridIndex(gridHelper.currentIndex());
  if (book == nullptr) return true;  // back tile / empty slot — nothing to edit

  // Capture by value: view_ pointers may be rebuilt while the picker is up.
  const uint32_t bookHash = book->pathHash;
  const auto mode = add ? CollectionPickerActivity::Mode::Add : CollectionPickerActivity::Mode::Remove;
  startActivityForResult(std::make_unique<CollectionPickerActivity>(renderer, mappedInput, bookHash, mode),
                         [this](const ActivityResult&) { onCollectionMembershipChanged(); });
  return true;  // picker launched (pushed) — close the global menu behind it
}

// ---- Input ------------------------------------------------------------------

void LibraryActivity::loop() {
  // Pick up the prefetch worker's batch-completion signal. After a page load
  // (loadPage) the caller painted placeholders; once the page's covers are
  // resident we clear the flag and repaint to swap them for real artwork.
  if (prefetchBatchDone_ != nullptr && xSemaphoreTake(prefetchBatchDone_, 0) == pdTRUE) {
    if (pendingPageRender_) {
      pendingPageRender_ = false;
      requestUpdate();
    }
  }

  if (rapidJumping_ && !mappedInput.isPressed(MappedInputManager::Button::Up) &&
      !mappedInput.isPressed(MappedInputManager::Button::Down)) {
    endRapidJumpIfActive();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSelect();
    return;
  }

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] { moveUp(); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] { moveDown(); });

  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this] { jumpPageBack(); });

  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this] { jumpPageForward(); });

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveLeft();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveRight();
    return;
  }
}

// ---- Navigation -------------------------------------------------------------

void LibraryActivity::moveUp() {
  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.up();
  const uint8_t newPage = gridHelper.currentPage();
  // On a page change, swap the cache to the new page; the requestUpdate below
  // paints placeholders immediately and the worker's batch-done repaints with
  // real covers. Within-page moves just repaint (covers already cached).
  if (oldPage != newPage) loadPage(newPage);
  requestUpdate();
}

void LibraryActivity::moveDown() {
  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.down();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) loadPage(newPage);
  requestUpdate();
}

void LibraryActivity::jumpPageForward() {
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  // During a held rapid-jump, suppress prefetch / SD work — we want the
  // render path to take the placeholder fallback for any uncached covers.
  // Page-aware retention + prefetch resumes when the button is released
  // (see the onContinuous binding's release handler).
  if (!rapidJumping_) {
    rapidJumping_ = true;
    cancelAllPrefetch();
  }
  lastObservedPage_ = gridHelper.currentPage();
  requestUpdate();
}

void LibraryActivity::jumpPageBack() {
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + pages - 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  if (!rapidJumping_) {
    rapidJumping_ = true;
    cancelAllPrefetch();
  }
  lastObservedPage_ = gridHelper.currentPage();
  requestUpdate();
}

void LibraryActivity::moveLeft() {
  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.left();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) loadPage(newPage);
  requestUpdate();
}

void LibraryActivity::moveRight() {
  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.right();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) loadPage(newPage);
  requestUpdate();
}

void LibraryActivity::moveNext() {
  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.nextItem();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) loadPage(newPage);
  requestUpdate();
}

void LibraryActivity::endRapidJumpIfActive() {
  if (!rapidJumping_) return;
  rapidJumping_ = false;

  // Hold ended — swap the cache to the landed page and repaint. The flick's
  // placeholders persist until the worker's batch-done fills in real covers.
  loadPage(gridHelper.currentPage());
  requestUpdate();
}

void LibraryActivity::doSelect() {
  const int idx = gridHelper.currentIndex();
  if (isBackTileIndex(idx)) {
    activateBack();
    return;
  }

  const LibraryBook* book = bookForGridIndex(idx);
  if (book == nullptr) return;

  LOG_DBG(LOG_TAG, "Opening book: %s", book->path.c_str());

  // The font caches are reset by ActivityManager on the Replace below, freeing
  // heap before the reader's (memory-hungry) onEnter — no explicit clear here.
  activityManager.goToReader(book->path);
}

bool LibraryActivity::onSearch() {
  const std::string current =
      (SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_SEARCH) ? std::string(SETTINGS.libraryViewName)
                                                                        : std::string();
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), current,
                                              sizeof(SETTINGS.libraryViewName) - 1, InputType::Text),
      [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          const auto& kb = std::get<KeyboardResult>(res.data);
          if (!kb.text.empty()) {
            SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_SEARCH;
            SETTINGS.libraryViewCollectionId = 0;
            strncpy(SETTINGS.libraryViewName, kb.text.c_str(), sizeof(SETTINGS.libraryViewName) - 1);
            SETTINGS.libraryViewName[sizeof(SETTINGS.libraryViewName) - 1] = '\0';
            SETTINGS.saveToFile();
            reloadActiveView();
          }
        }
        requestUpdate();
      });
  return true;
}

void LibraryActivity::reloadActiveView() {
  cancelAllPrefetch();
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  rebuildView();
  xSemaphoreGive(cacheLock_);

  // Non-All views insert a back tile at slot 0, so default selection to the
  // first real book (slot 1); All Books starts at slot 0.
  const int initialIndex = (SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_ALL) ? 0 : 1;
  gridHelper = GridHelper(viewItemCount(), ROWS, COLS, initialIndex);
  lastObservedPage_ = 0;
  loadPage(0);  // caller repaints; placeholders show until the new view's covers load
}

void LibraryActivity::onCollectionMembershipChanged() {
  // Only a collection view's contents depend on membership. Rebuild it so an
  // added/removed book appears/disappears (reloadActiveView swaps the cache);
  // other views (All Books, auto-groups) are unaffected. Repaint either way: a
  // rebuilt view shows placeholders until covers load, an unchanged view paints
  // its still-cached covers.
  if (SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_COLLECTION) {
    COLLECTION_STORE.loadFromFile();
    reloadActiveView();
  }
  requestUpdate();
}

void LibraryActivity::onReturnFromCollections(bool viewChanged) {
  // The picker may have switched the active view (or created collections). When
  // it changed, reloadActiveView swaps the cache (placeholders until covers
  // load); otherwise preserve grid position. Repaint either way.
  if (viewChanged) reloadActiveView();
  requestUpdate();
}

void LibraryActivity::applySort() {
  cancelAllPrefetch();

  // Hold cacheLock_ across sortBy. The worker reads LIBRARY_INDEX from
  // inside buildThumbPath (called under cacheLock_), so sorting outside
  // the lock would race with a worker that won the lock between
  // cancelAllPrefetch's release and our sortBy.
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                       static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));
  // sortBy reorders the index in place, invalidating view_'s pointers — rebuild
  // the filtered view (same membership, new order) under the same lock.
  rebuildView();
  xSemaphoreGive(cacheLock_);

  this->gridHelper = GridHelper(viewItemCount(), ROWS, COLS, 0);
  lastObservedPage_ = 0;
  loadPage(0);
  requestUpdate();  // placeholders now; real covers on batch-done
}

void LibraryActivity::setSort(uint8_t field, uint8_t direction) {
  const bool changed = (field != SETTINGS.librarySortField) || (direction != SETTINGS.librarySortDirection);

  if (changed) {
    SETTINGS.librarySortField = field;
    SETTINGS.librarySortDirection = direction;
    SETTINGS.saveToFile();
  }

  applySort();
}

// ---- Active view ------------------------------------------------------------

int LibraryActivity::viewItemCount() const { return static_cast<int>(view_.size()) + (hasBackTile_ ? 1 : 0); }

bool LibraryActivity::isBackTileIndex(int gridIndex) const { return hasBackTile_ && gridIndex == 0; }

const LibraryBook* LibraryActivity::bookForGridIndex(int gridIndex) const {
  if (gridIndex < 0) return nullptr;
  const int bookIdx = gridIndex - (hasBackTile_ ? 1 : 0);
  if (bookIdx < 0 || bookIdx >= static_cast<int>(view_.size())) return nullptr;
  return view_[bookIdx];
}

void LibraryActivity::rebuildView() {
  view_.clear();
  hasBackTile_ = false;

  const auto& books = LIBRARY_INDEX.getBooks();
  view_.reserve(books.size());

  uint8_t kind = SETTINGS.libraryViewKind;

  // Validate a COLLECTION view up front: a stale id falls back to All Books.
  const Collection* coll = nullptr;
  if (kind == CrossPointSettings::LIB_VIEW_COLLECTION) {
    if (!COLLECTION_STORE.isLoaded()) COLLECTION_STORE.loadFromFile();
    coll = COLLECTION_STORE.findById(SETTINGS.libraryViewCollectionId);
    if (coll == nullptr) kind = CrossPointSettings::LIB_VIEW_ALL;
  }

  auto fallBackToAll = [&]() {
    view_.clear();
    for (const auto& b : books) view_.push_back(&b);
    hasBackTile_ = false;
    viewTitle_ = tr(STR_LIBRARY);
    if (SETTINGS.libraryViewKind != CrossPointSettings::LIB_VIEW_ALL) {
      SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_ALL;
      SETTINGS.libraryViewCollectionId = 0;
      SETTINGS.libraryViewName[0] = '\0';
      SETTINGS.saveToFile();
    }
  };

  if (kind == CrossPointSettings::LIB_VIEW_ALL) {
    fallBackToAll();
    return;
  }

  if (kind == CrossPointSettings::LIB_VIEW_COLLECTION) {
    const std::unordered_set<uint32_t> members(coll->members.begin(), coll->members.end());
    for (const auto& b : books) {
      if (members.count(b.pathHash)) view_.push_back(&b);
    }
    hasBackTile_ = true;
    viewTitle_ = coll->name;  // empty collection is allowed (back-tile-only view)
    return;
  }

  if (kind == CrossPointSettings::LIB_VIEW_SEARCH) {
    const std::string query = SETTINGS.libraryViewName;
    if (query.empty()) {
      fallBackToAll();
      return;
    }
    // No-results is a valid state: keep the back tile so the user can return.
    view_ = LIBRARY_INDEX.search(query);
    hasBackTile_ = true;
    viewTitle_ = query;
    return;
  }

  // Auto-group: series / author / genre by exact name match.
  const std::string name = SETTINGS.libraryViewName;
  for (const auto& b : books) {
    // Genre holds multiple newline-joined subjects: a book belongs to the group
    // if any of its subjects matches the selected genre name.
    if (kind == CrossPointSettings::LIB_VIEW_GENRE) {
      bool match = false;
      forEachGenre(b.genre, [&](std::string_view g) {
        if (g == name) match = true;
      });
      if (match) view_.push_back(&b);
      continue;
    }
    const std::string* field = nullptr;
    switch (kind) {
      case CrossPointSettings::LIB_VIEW_SERIES:
        field = &b.series;
        break;
      case CrossPointSettings::LIB_VIEW_AUTHOR:
        field = &b.author;
        break;
    }
    if (field != nullptr && *field == name) view_.push_back(&b);
  }

  if (view_.empty()) {
    // The group no longer matches any book (e.g. the book was removed) — reset.
    fallBackToAll();
    return;
  }

  hasBackTile_ = true;
  viewTitle_ = name;
}

void LibraryActivity::activateBack() {
  SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_ALL;
  SETTINGS.libraryViewCollectionId = 0;
  SETTINGS.libraryViewName[0] = '\0';
  SETTINGS.saveToFile();

  cancelAllPrefetch();
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  rebuildView();
  xSemaphoreGive(cacheLock_);

  this->gridHelper = GridHelper(viewItemCount(), ROWS, COLS, 0);
  lastObservedPage_ = 0;
  loadPage(0);
  requestUpdate();  // placeholders now; real covers on batch-done
}

// ---- Render -----------------------------------------------------------------

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderPasses();
  renderer.displayBuffer();
}

Rect LibraryActivity::computeCoverSlot() const {
  // Replay the render path's layout (UIPage body → shelf Hstack → cell Grid →
  // tile Vstack) without drawing, so the worker scales covers to exactly the
  // dims the render path will request. Cells are uniform → cell[0] is
  // representative. Mirrors UIPage::render + renderPasses + renderBookTile; keep
  // in sync if those change.
  const auto& td = *GUI.getData();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};

  flex::Vstack page(screen, {flex::fixed(td.layout.topPadding), flex::fixed(td.header.height), flex::grow(),
                             flex::fixed(td.buttonHints.height)});
  const Rect body = flex::inset(page[2], flex::xy(td.library.pagePaddingX, td.library.pagePaddingY));

  flex::Hstack bodyInner(body, {flex::grow(), flex::fixed(RAIL_WIDTH)}, RAIL_GAP,
                         flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y));
  flex::Grid cells(bodyInner[0], ROWS, COLS, CELL_GAP, CELL_GAP);

  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};
  flex::Vstack tile(cells[0], {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()}, 0,
                    tilePad);
  return tile[0];
}

void LibraryActivity::renderPasses() {
  const auto& td = *GUI.getData();

  // Theme/orientation can change the cover-slot size between paints. When it
  // does, re-seed the draw envelope and reload the page at the new dims. This
  // runs mid-paint, so this frame paints placeholders for the just-cleared
  // covers; batch-done then repaints with correctly-sized rasters (rare path).
  if (cacheLock_ != nullptr) {
    const Rect cs = computeCoverSlot();
    if (cs.width != coverBoxW_ || cs.height != coverBoxH_) {
      coverBoxW_ = cs.width;
      coverBoxH_ = cs.height;
      // No requestUpdate here — we're on the render task mid-paint. This frame
      // paints placeholders for the just-cleared covers; the worker's batch-done
      // triggers the real repaint from loop() (main task).
      loadPage(gridHelper.currentPage());
    }
  }

  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const auto btnLabels =
      mappedInput.mapLabels(tr(STR_MENU_LABEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));

  const auto body = UIPage::render(renderer, viewTitle_.empty() ? tr(STR_LIBRARY) : viewTitle_.c_str(),
                                   getHeaderSubtitleText().c_str(), btnLabels,
                                   flex::xy(td.library.pagePaddingX, td.library.pagePaddingY));

  // Zero grid items means an empty device library (the All view with no
  // books). A filtered-but-empty view still has its back tile, so it falls
  // through to the shelf and renders just that tile.
  if (viewItemCount() == 0) {
    renderEmptyState(body);
  } else {
    flex::Hstack bodyInner(body, {flex::grow(), flex::fixed(RAIL_WIDTH)}, RAIL_GAP,
                           flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y));
    {
      renderLibraryShelf(bodyInner[0]);
      renderPageRail(bodyInner[1]);
    }
  }
}

bool LibraryActivity::handlePowerShortPress() {
  switch (SETTINGS.libraryPowerButton) {
    case CrossPointSettings::LIB_PWR_NEXT_BOOK:
      moveNext();
      break;
    case CrossPointSettings::LIB_PWR_NEXT_IN_ROW:
      moveRight();
      break;
  }

  return true;
}

void LibraryActivity::renderBattery(const Rect& headerBox) {
  const auto& td = *GUI.getData();
  constexpr int kBatteryEraseWidth = 80;
  constexpr int kBatteryTopOffset = 5;
  constexpr int kBatteryRightInset = 12;

  const Rect batteryRow{headerBox.x, headerBox.y + kBatteryTopOffset, headerBox.width - kBatteryRightInset,
                        td.battery.height};

  const Rect batteryRect =
      flex::align(batteryRow, td.battery.width, td.battery.height, flex::HAlign::End, flex::VAlign::Start);

  const bool showBatteryPct =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;

  GUI.drawBatteryRight(renderer, batteryRect, showBatteryPct);
}

std::string LibraryActivity::getHeaderSubtitleText() {
  if (viewItemCount() == 0) {
    return std::string("");
  }

  StrId sortedKey = StrId::STR_LIBRARY_SORTED_RECENT;
  switch (SETTINGS.librarySortField) {
    case CrossPointSettings::LIB_SORT_TITLE:
      sortedKey = StrId::STR_LIBRARY_SORTED_TITLE;
      break;
    case CrossPointSettings::LIB_SORT_AUTHOR:
      sortedKey = StrId::STR_LIBRARY_SORTED_AUTHOR;
      break;
    case CrossPointSettings::LIB_SORT_PROGRESS:
      sortedKey = StrId::STR_LIBRARY_SORTED_PROGRESS;
      break;
    case CrossPointSettings::LIB_SORT_RECENT:
    default:
      sortedKey = StrId::STR_LIBRARY_SORTED_RECENT;
      break;
  }

  const char* arrow = (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC) ? "(asc)" : "(desc)";

  return std::string(I18n::getInstance().get(sortedKey)) 
    + " " 
    + arrow 
    + "  ·  " 
    + std::to_string(view_.size()) 
    + " " 
    + tr(STR_BOOKS);
}

void LibraryActivity::renderLibraryShelf(const Rect& shelfArea) {
  const uint8_t currentIndexOnPage = gridHelper.currentIndexOnPage();
  const uint8_t currentPage = gridHelper.currentPage();
  const uint8_t itemsPerPage = gridHelper.itemsPerPage();
  const uint16_t baseIndex = currentPage * itemsPerPage;

  flex::Grid cells(shelfArea, ROWS, COLS, CELL_GAP, CELL_GAP);
  {
    for (int slot = 0; slot < PER_PAGE; ++slot) {
      const int idx = baseIndex + slot;
      if (isBackTileIndex(idx)) {
        renderBackTile(cells[slot], slot == currentIndexOnPage);
        continue;
      }
      const LibraryBook* book = bookForGridIndex(idx);
      if (book == nullptr) continue;
      renderBookTile(cells[slot], *book, slot == currentIndexOnPage);
    }
  }
}

void LibraryActivity::renderBookTile(const Rect& cell, const LibraryBook& book, bool selected) {
  const bool invertText = selected && GUI.getData()->selection.textInverted;
  const bool textBlack = !invertText;

  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  constexpr int kProgressGap = 8;

  // ---- Tile interior bands ----
  // Vstack padding {top=kTilePadTop, bottom=kTilePadBottom}. Inside that span:
  //   * cover  — kCoverPercent (60%) of inner — Cover::render aspect-fits
  //              the thumb; small fallback frame uses Cover::kFallback*.
  //   * text   — grows to fill the rest (absorbs the progress bands when the
  //              book has no progress); the title/author render top-anchored
  //              at the slot top so titles line up across the grid.
  //   * gap + progress — only when book.hasProgress(); ProgressBar's
  //                       intrinsic height drives the slot, pinned to the
  //                       inner bottom so bars line up across the grid.
  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};

  Rect coverSlot;
  Rect textSlot;
  Rect progressSlot;

  if (book.hasProgress()) {
    flex::Vstack tile(cell,
                      {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow(),
                       flex::fixed(kProgressGap), flex::fixed(ProgressBar::kIntrinsicHeight)},
                      0, tilePad);

    coverSlot = tile[0];
    textSlot = tile[2];
    progressSlot = tile[4];
  } else {
    flex::Vstack tile(cell, {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()}, 0, tilePad);

    coverSlot = tile[0];
    textSlot = tile[2];
  }

  // Measure the title/author stack before drawing the frame: the selection
  // frame hugs the actual content bottom (progress-bar bottom for progress
  // books; the author/title bottom otherwise).
  const int authorReserved = book.author.empty() ? 0 : (kTitleAuthorGap + captionLineH);
  const int titleBudget = textSlot.height - authorReserved;
  const int maxTitleLines = std::min(2, std::max(1, titleBudget / captionLineH));

  const std::vector<std::string> titleLines =
      renderer.wrappedText(captionFont, book.title.c_str(), textSlot.width - 8, maxTitleLines, EpdFontFamily::BOLD);

  const std::string authorTrunc =
      book.author.empty()
          ? std::string()
          : renderer.truncatedText(captionFont, book.author.c_str(), textSlot.width - 8, EpdFontFamily::ITALIC);

  // Tight, top-anchored text box: TextBlock vertically centers within the box,
  // so a box sized to the natural text height places the text at the slot top.
  const int textH =
      static_cast<int>(titleLines.size()) * captionLineH + (authorTrunc.empty() ? 0 : (kTitleAuthorGap + captionLineH));

  const Rect textBox = flex::center(textSlot, textSlot.width, textH);

  const int contentBottom =
      book.hasProgress() ? progressSlot.y + ProgressBar::kIntrinsicHeight : textBox.y + textBox.height;

  Rect selectionRect{};
  if (selected) {
    selectionRect = computeSelectionFrame(cell, contentBottom);
    GUI.drawSelectionBackground(renderer, selectionRect);
  }

  {
    char thumbPath[64];
    snprintf(thumbPath, sizeof(thumbPath), "/.crosspoint/epub_%lu/thumb_%d.bmp",
             static_cast<unsigned long>(book.pathHash), LibraryIndex::THUMB_HEIGHT);

    xSemaphoreTake(cacheLock_, portMAX_DELAY);
    {
      const Cover::Fallback coverFallback{book.title.c_str(), captionFont, EpdFontFamily::BOLD};
      Cover::render(renderer, coverSlot, pageCache_, thumbPath, invertText, coverSlot.height * 2 / 3, coverSlot.height,
                    &coverFallback);
    }
    xSemaphoreGive(cacheLock_);

    TextBlock::Line tlines[3];  // up to 2 title lines + 1 author line
    std::size_t tcount = 0;
    for (const auto& l : titleLines) {
      tlines[tcount++] = TextBlock::Line{l.c_str(), captionFont, EpdFontFamily::BOLD, 0, false};
    }
    if (!authorTrunc.empty()) {
      tlines[tcount++] =
          TextBlock::Line{authorTrunc.c_str(), captionFont, EpdFontFamily::ITALIC, kTitleAuthorGap, false};
    }

    TextBlock::render(renderer, textBox, tlines, tcount, textBlack);

    if (book.hasProgress()) {
      const Rect barBox = flex::align(progressSlot, Cover::kFallbackWidth, ProgressBar::kIntrinsicHeight,
                                      flex::HAlign::Center, flex::VAlign::Start);
      ProgressBar::render(renderer, barBox, book.progressPercent(), textBlack);
    }
  }

  // ---- Selection foreground pass (drawn over tile content) ----
  if (selected) {
    GUI.drawSelectionForeground(renderer, selectionRect);
  }
}

void LibraryActivity::renderBackTile(const Rect& cell, bool selected) {
  const bool invertText = selected && GUI.getData()->selection.textInverted;
  const bool textBlack = !invertText;

  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};

  // Same geometry as a bar-less book tile so the back tile aligns with the grid.
  flex::Vstack tile(cell, {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()}, 0, tilePad);
  const Rect coverSlot = tile[0];
  const Rect textSlot = tile[2];

  const std::string labelTrunc =
      renderer.truncatedText(captionFont, tr(STR_ALL_BOOKS), textSlot.width - 8, EpdFontFamily::BOLD);
  const Rect textBox{textSlot.x, textSlot.y, textSlot.width, captionLineH};
  const int contentBottom = textBox.y + textBox.height;

  Rect selectionRect{};
  if (selected) {
    selectionRect = computeSelectionFrame(cell, contentBottom);
    GUI.drawSelectionBackground(renderer, selectionRect);
  }

  // A bordered box (fallback-cover sized) with a centered left arrow.
  const Rect box =
      flex::align(coverSlot, Cover::kFallbackWidth, Cover::kFallbackHeight, flex::HAlign::Center, flex::VAlign::Start);
  renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 2, 6, textBlack);
  const int cx = box.x + box.width / 2;
  const int cy = box.y + box.height / 2;
  const int arm = box.width / 4;
  const int barb = box.height / 8;
  renderer.drawLine(cx - arm, cy, cx + arm, cy, 3, textBlack);                // shaft
  renderer.drawLine(cx - arm, cy, cx - arm + barb, cy - barb, 3, textBlack);  // upper barb
  renderer.drawLine(cx - arm, cy, cx - arm + barb, cy + barb, 3, textBlack);  // lower barb

  TextBlock::Line line{labelTrunc.c_str(), captionFont, EpdFontFamily::BOLD, 0, false};
  TextBlock::render(renderer, textBox, &line, 1, textBlack);

  if (selected) {
    GUI.drawSelectionForeground(renderer, selectionRect);
  }
}

void LibraryActivity::renderPageRail(const Rect& railArea) {
  const int pages = std::max(1, static_cast<int>(gridHelper.pageCount()));
  const int railTop = railArea.y + RAIL_PAD_TOP;
  const int railBottom = railArea.y + railArea.height;

  const auto& lib = GUI.getData()->library;
  const int tickSize = lib.pageIndicatorSize;
  const uint8_t currentPage = this->gridHelper.currentPage();

  for (int i = 0; i < pages; ++i) {
    const int tickRowY = railTop + i * (tickSize + lib.pageIndicatorGap);
    // Per-tick row slot spans the full rail width; the tick itself is
    // centered horizontally inside it via flex::align.
    const Rect tickRow{railArea.x, tickRowY, railArea.width, tickSize};
    const Rect tick = flex::align(tickRow, tickSize, tickSize, flex::HAlign::Center, flex::VAlign::Start);

    const Color fill = (i == currentPage) ? lib.pageIndicatorFillSelected : lib.pageIndicatorFill;
    const Color border = (i == currentPage) ? lib.pageIndicatorBorderSelected : lib.pageIndicatorBorder;

    switch (lib.pageIndicatorShape) {
      case IndicatorShape::Circle: {
        const int r = tickSize / 2;
        renderer.fillCircle(tick.x + r, tick.y + r, r, fill);
        renderer.drawCircle(tick.x + r, tick.y + r, r, border);
        break;
      }
      case IndicatorShape::Square:
        renderer.fillRectDither(tick.x, tick.y, tick.width, tick.height, fill);
        renderer.drawRect(tick.x, tick.y, tick.width, tick.height, border == Color::Black);
        break;
      case IndicatorShape::RoundedRect:
        renderer.fillRoundedRect(tick.x, tick.y, tick.width, tick.height, lib.pageIndicatorCornerRadius, fill);
        renderer.drawRoundedRect(tick.x, tick.y, tick.width, tick.height, 1, lib.pageIndicatorCornerRadius,
                                 border == Color::Black);
        break;
    }
  }

  // Page count at the bottom of the rail
  const int accentFont = libFont(FontRole::AccentCompact);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", currentPage + 1, pages);
  const int countY = railBottom - 4;
  const int countX = railArea.x + railArea.width / 2 + 4;
  renderer.drawTextRotated90CW(accentFont, countX, countY, countBuf, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderEmptyState(const Rect& body) {
  // "No books on SD card" — italic heading, centered both axes inside the
  // body region.
  const int font = libFont(FontRole::Heading);
  const char* msg = tr(STR_LIBRARY_NO_BOOKS);
  const int textW = renderer.getTextWidth(font, msg, EpdFontFamily::ITALIC);
  const int textH = renderer.getLineHeight(font);
  const Rect at = flex::center(body, textW, textH);
  renderer.drawText(font, at.x, at.y, msg, true, EpdFontFamily::ITALIC);
}

bool LibraryActivity::onSortSelect(CrossPointSettings::LIBRARY_SORT_FIELD sortType) {
  uint8_t direction = SETTINGS.librarySortDirection;
  if (sortType == SETTINGS.librarySortField) {
    // Re-selecting the active field toggles its direction.
    direction = (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
                    ? CrossPointSettings::LIB_SORT_DESC
                    : CrossPointSettings::LIB_SORT_ASC;
  }
  setSort(sortType, direction);
  return false;
}

std::optional<PopupMenu::Glyph> LibraryActivity::getSortGlyph(CrossPointSettings::LIBRARY_SORT_FIELD sortType) {
  if(sortType == SETTINGS.librarySortField) {
    return SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC
      ? PopupMenu::Glyph::ArrowUp
      : PopupMenu::Glyph::ArrowDown;
  }

  return std::nullopt;
}

std::vector<MenuRegistryEntry> LibraryActivity::getGlobalMenuEntries() {
  using SortField = CrossPointSettings::LIBRARY_SORT_FIELD;

  auto entries = std::vector<MenuRegistryEntry>{
    MenuRegistryEntry{
      .icon = { 40, 40, Sort40Icon },
      .name = tr(STR_SORT),
      .popupItems = {
        PopupMenuEntry{
          .label = tr(STR_SORT_RECENT),
          .glyph = getSortGlyph(SortField::LIB_SORT_RECENT),
          .onSelected = [this]() { return this->onSortSelect(SortField::LIB_SORT_RECENT); }
        },
        PopupMenuEntry{
          .label = tr(STR_SORT_TITLE),
          .glyph = getSortGlyph(SortField::LIB_SORT_TITLE),
          .onSelected = [this]() { return this->onSortSelect(SortField::LIB_SORT_TITLE); }
        },
        PopupMenuEntry{
          .label = tr(STR_SORT_AUTHOR),
          .glyph = getSortGlyph(SortField::LIB_SORT_AUTHOR),
          .onSelected = [this]() { return this->onSortSelect(SortField::LIB_SORT_AUTHOR); }
        },
        PopupMenuEntry{
          .label = tr(STR_SORT_PROGRESS),
          .glyph = getSortGlyph(SortField::LIB_SORT_PROGRESS),
          .onSelected = [this]() { return this->onSortSelect(SortField::LIB_SORT_PROGRESS); }
        },
      }
    },
    MenuRegistryEntry{
      .icon = { 40, 40, Books40Icon },
      .name = tr(STR_COLLECTIONS),
      .onPress = [this]() {
        const uint8_t prevKind = SETTINGS.libraryViewKind;
        const uint32_t prevId = SETTINGS.libraryViewCollectionId;
        const std::string prevName = SETTINGS.libraryViewName;
        startActivityForResult(std::make_unique<CollectionsActivity>(renderer, mappedInput),
                               [this, prevKind, prevId, prevName](const ActivityResult&) {
                                 const bool changed = SETTINGS.libraryViewKind != prevKind ||
                                                      SETTINGS.libraryViewCollectionId != prevId ||
                                                      prevName != std::string(SETTINGS.libraryViewName);
                                 onReturnFromCollections(changed);
                               });
        return true;
      }
    },
    MenuRegistryEntry{
      .icon = { 40, 40, Search40Icon },
      .name = tr(STR_SEARCH),
      .onPress = [this]() { return onSearch(); }
    }
  };

  const int idx = gridHelper.currentIndex();
  if (!isBackTileIndex(idx)) {
    entries.insert(entries.begin(), MenuRegistryEntry{
      .icon = { 40, 40, Bookalt40Icon },
      .name = tr(STR_SELECTED_BOOK),
      .popupItems = {
        PopupMenuEntry{
          .label = tr(STR_ADD_TO_COLLECTION),
          .onSelected = [this]() { return openCollectionPicker(/*add=*/true); }
        },
        PopupMenuEntry{
          .label = tr(STR_REMOVE_FROM_COLLECTION),
          .onSelected = [this]() { return openCollectionPicker(/*add=*/false); }
        }
      }
    });
  }

  return entries;
}
