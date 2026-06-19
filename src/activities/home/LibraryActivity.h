#pragma once
#include <BitmapCacheManager.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LibraryIndex.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect
#include "util/ButtonNavigator.h"
#include "util/GridHelper.h"
#include "CrossPointSettings.h"

class LibraryActivity final : public Activity {
 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;

  // Worst-case 2bpp source for one thumbnail (THUMB_MAX_WIDTH × THUMB_HEIGHT,
  // stride = (w+3)/4). Reused decode scratch for the prefetch worker.
  static constexpr std::size_t DECODE_SCRATCH_BYTES =
      static_cast<std::size_t>((LibraryIndex::THUMB_MAX_WIDTH + 3) / 4) * LibraryIndex::THUMB_HEIGHT;

  // Dedicated raster region for the cover cache. Worst case is one page of 9
  // covers × (1bpp 120×120 = (120+7)/8 × 120 = 1800 B) = 16.2 KB + ~0.1 KB block
  // headers; 20 KB leaves margin for intra-arena fragmentation. The page is
  // cleared and reloaded as a unit, so the free list coalesces cleanly and an
  // over-budget raster simply renders a placeholder (no crash). All cover buffer
  // churn lives here instead of fragmenting the general heap.
  static constexpr std::size_t COVER_ARENA_BYTES = 20 * 1024;

  // ---- View state ---------------------------------------------------------
  // GridHelper
  GridHelper gridHelper = GridHelper(0, 0, 0, 0);

  // The active library view (All Books vs. a collection / auto-group filter).
  // view_ holds pointers into LIBRARY_INDEX in sorted order, filtered to the
  // active view. Rebuilt after every sort and on entry. When the view is
  // filtered, a synthetic "back tile" occupies grid slot 0 (page 0) — so the
  // grid item count is view_.size() + (hasBackTile_ ? 1 : 0). Pointers are
  // only valid between sorts (sortBy reorders the index in place), so view_
  // is always rebuilt immediately after sortBy.
  std::vector<const LibraryBook*> view_;
  bool hasBackTile_ = false;
  // Cached header title for the active view ("Library" for All Books,
  // otherwise the collection / series / author / genre name).
  std::string viewTitle_;

  // Drives Up/Down hold-to-page-jump (every 500ms after a 500ms hold) and
  // suppresses the tap callback when a continuous fired. Default 500/500
  // matches the cadence used by every settings list in the app.
  ButtonNavigator buttonNavigator;

  // True when the Confirm release that brought us into this activity should
  // not also trigger an open-book (typical when launched from a parent menu).
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Fixed-capacity cover cache sized to ONE page of thumbs, backed by its own
  // arena (COVER_ARENA_BYTES). The async prefetch worker decodes + pre-scales
  // each cover to its draw dimensions and stores only the 1bpp raster. On a page
  // change we clear the cache and load the new page before rendering it (no
  // placeholder flash); placeholders appear only while a rapid-jump (Up/Down
  // hold) flicks through pages. Entries hold no 2bpp source; the worker reuses a
  // single decode scratch (decodeScratch_).
  BitmapCacheManager pageCache_{PER_PAGE, COVER_ARENA_BYTES};

  // Reused 2bpp decode scratch for the prefetch worker (allocated in onEnter,
  // freed in onExit). Single-producer: only the worker writes/reads it.
  std::unique_ptr<uint8_t[]> decodeScratch_;

  // Cover draw dimensions (the grid's cover-slot W/H), computed from the flex
  // layout in onEnter and refreshed on theme/orientation change. The worker
  // pre-scales each cover to fit this envelope via GfxRenderer::computeThumbTarget.
  int coverBoxW_ = 0;
  int coverBoxH_ = 0;

  // ---- Prefetch worker state ---------------------------------------------
  // FreeRTOS task that decodes neighbor-page covers off the render path.
  // Created in onEnter, joined and destroyed in onExit. Communication:
  //   - cacheLock_ guards pageCache_, prefetchQueue_, and the queue count.
  //   - prefetchSignal_ is a binary semaphore the worker waits on.
  //   - prefetchCancel_ is a volatile flag the worker checks between
  //     covers to abandon its current scan (set on page change / sort /
  //     rapid-jump / shutdown).
  //   - prefetchShutdown_ tells the worker to exit cleanly.
  // Owner: activity thread enqueues page indices; worker dequeues and, for each
  // cover, decodes into decodeScratch_ off-lock, then (under cacheLock_) scales
  // to the draw dims and stores the 1bpp raster via GfxRenderer + cache.set.
  TaskHandle_t prefetchTask_ = nullptr;
  SemaphoreHandle_t cacheLock_ = nullptr;
  SemaphoreHandle_t prefetchSignal_ = nullptr;
  // Given by the worker after its main loop has exited. onExit waits on
  // this before calling vTaskDelete, ensuring the worker isn't holding
  // cacheLock_ or mid-SD-decode when it's deleted.
  SemaphoreHandle_t prefetchExited_ = nullptr;
  // Given by the worker after each fully-completed page batch (not on
  // cancel). Activity loop polls it to clear pendingPageRender_ and
  // trigger the deferred repaint once the awaited page lands in the cache.
  SemaphoreHandle_t prefetchBatchDone_ = nullptr;
  // Only ever one page is in flight (we clear + load a single page on each
  // settle), but a release mid-decode can enqueue a second before the worker
  // drains the first. Sentinel 0xFF = empty slot.
  static constexpr std::size_t PREFETCH_QUEUE_CAPACITY = 4;
  uint8_t prefetchQueue_[PREFETCH_QUEUE_CAPACITY] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t prefetchQueueCount_ = 0;
  volatile bool prefetchCancel_ = false;
  volatile bool prefetchShutdown_ = false;

  // gridHelper.currentPage() as last loaded, for bookkeeping.
  uint8_t lastObservedPage_ = 0;
  // True while a rapid-jump (continuous Up/Down) hold is in progress. We don't
  // decode pages flicked past, so the render path peeks and shows placeholders
  // for the uncached page; release (endRapidJumpIfActive) loads the landed page
  // for real. Volatile because the render task reads it without cacheLock_.
  volatile bool rapidJumping_ = false;
  // Set by loadPage: a page load is in flight. The caller repaints immediately
  // (placeholders for the not-yet-decoded covers); when the worker signals
  // batch-done the activity loop clears this and repaints again to swap the
  // placeholders for the now-resident covers.
  volatile bool pendingPageRender_ = false;

  // ---- Prefetch helpers --------------------------------------------------
  static void prefetchTaskTrampoline(void* ctx);
  void prefetchTaskLoop();
  // Build the SD path for the cover at (page, slot). Returns false if no
  // book occupies that grid position (past end of library). out must be
  // at least 64 bytes.
  bool buildThumbPath(uint8_t page, uint8_t slot, char* out, std::size_t outSize) const;
  // Enqueue a page for prefetch. Validates the page index, dedupes against
  // the existing queue, signals the worker. Caller holds cacheLock_. Returns
  // false only when the page is out of range (nothing to load).
  bool enqueuePrefetchLocked(uint8_t page);
  // Cancel any in-flight or queued prefetch. Acquires cacheLock_.
  void cancelAllPrefetch();
  // Swap the (single-page) cache to `page`: cancel any in-flight prefetch, clear
  // the cache, enqueue `page`, drain any stale batch-done, and set
  // pendingPageRender_. Does NOT requestUpdate itself — main-loop callers paint
  // placeholders immediately (peek misses the cleared cache) and the worker's
  // batch-done repaints with real covers (loop()). Lazy load: the page turn is
  // instant (placeholders) and covers fill in shortly after.
  void loadPage(uint8_t page);
  // Launch the collection-membership picker for the book under the cursor.
  // Returns true (close the popup) when there's no book to edit; false while
  // the picker is launched (the return handler closes the popup on resume).
  bool openCollectionPicker(bool add);

  void moveUp();
  void moveDown();
  void moveLeft();
  void moveRight();
  void moveNext();
  void jumpPageBack();
  void jumpPageForward();
  // Resume normal prefetch + render-side cache loading after a rapid-jump
  // continuous-hold ends. No-op if not currently rapid-jumping.
  void endRapidJumpIfActive();

  // ---- Active view helpers ------------------------------------------------
  // Read the persisted active view from settings, validate it (a missing
  // collection or an empty auto-group falls back to All Books and resaves),
  // then rebuild view_ / hasBackTile_ / viewTitle_ from the (sorted) index.
  // Call after every sortBy and on entry.
  void rebuildView();
  // Number of grid items in the active view (filtered books + back tile).
  int viewItemCount() const;
  // True when gridIndex addresses the synthetic back tile (slot 0, filtered).
  bool isBackTileIndex(int gridIndex) const;
  // The book at a grid index, or nullptr for the back tile / out of range.
  const LibraryBook* bookForGridIndex(int gridIndex) const;
  // Switch back to All Books (used by the back tile), persist, and repaint.
  void activateBack();
  void renderBackTile(const Rect& cell, bool selected);

  void doSelect();
  // Launch the keyboard to enter a search query; on confirm, switch the shelf to
  // the LIB_VIEW_SEARCH filter for that query. Returns true (closes the popup).
  bool onSearch();
  // Close the popup and refresh after the collection-membership picker returns.
  // Only a collection view's contents depend on membership, so the view is
  // rebuilt only then; otherwise the grid is just repainted in place.
  void onCollectionMembershipChanged();
  // Refresh after the (suspended) CollectionsActivity returns. Rebuilds the
  // active view only when it changed, preserving grid position otherwise.
  void onReturnFromCollections(bool viewChanged);
  // Rebuild view_ from current settings and reset the grid to the view's
  // default selection (shared by the membership / collections return paths).
  void reloadActiveView();
  // Swallow the trailing Confirm/Back release after a suspended sub-activity
  // returns, so it doesn't fall through to doSelect() / popup re-open.
  void lockSubActivityReturnRelease();
  // Re-sort the index from current settings and reset selection to the top.
  void applySort();
  // Persist the active sort field/direction and re-sort.
  void setSort(uint8_t field, uint8_t direction);

  // ---- Rendering helpers --------------------------------------------------
  // Compute the cover-slot rect for a grid cell by replaying the same flex
  // layout the render path uses (no drawing). Cells are uniform, so this is the
  // draw envelope for every cover. Used to seed coverBoxW_/H_ and to detect
  // theme/orientation layout changes.
  Rect computeCoverSlot() const;

  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderBattery(const Rect& headerBox);
  std::string getHeaderSubtitleText();
  void renderLibraryShelf(const Rect& shelfArea);
  void renderPageRail(const Rect& railArea);
  void renderBookTile(const Rect& cell, const LibraryBook& book, bool selected);
  void renderEmptyState(const Rect& body);
  bool onSortSelect(CrossPointSettings::LIBRARY_SORT_FIELD sortType);
  std::optional<PopupMenu::Glyph> getSortGlyph(CrossPointSettings::LIBRARY_SORT_FIELD sortType);

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Power-button override: short-press advances linearly through the
  // library (delegates to moveNext()), wrapping at the end.
  bool handlePowerShortPress() override;

  std::vector<MenuRegistryEntry> getGlobalMenuEntries() override;
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    return GlobalMenuConfig{.clearFontCacheOnClose = false};
  }
};
