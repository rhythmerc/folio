#pragma once
#include <BitmapCacheManager.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LibraryIndex.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"
#include "util/ButtonNavigator.h"
#include "util/GridHelper.h"

class LibraryActivity final : public Activity {
 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;

  // Top-panel row indices for the cascading popup. The activity dispatches
  // leaf actions and sort logic against these indices.
  static constexpr int POPUP_TOP_SORT = 0;
  static constexpr int POPUP_TOP_COLLECTIONS = 1;  // leaf → CollectionsActivity
  static constexpr int POPUP_TOP_BOOK = 2;         // submenu: Add / Remove from Collection
  static constexpr int POPUP_TOP_FILES = 3;
  static constexpr int POPUP_TOP_POWER = 4;
  static constexpr int POPUP_TOP_SETTINGS = 5;
  static constexpr int POPUP_TOP_COUNT = 6;

  static constexpr int POPUP_BOOK_ADD = 0;
  static constexpr int POPUP_BOOK_REMOVE = 1;
  static constexpr int POPUP_BOOK_COUNT = 2;

  static constexpr int POPUP_FILES_BROWSE = 0;
  static constexpr int POPUP_FILES_TRANSFER = 1;
  static constexpr int POPUP_FILES_COUNT = 2;

  static constexpr int POPUP_SORT_COUNT = 4;   // Recent / Title / Author / Progress
  static constexpr int POPUP_POWER_COUNT = 2;  // Next in Row / Next Book

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

  // The cascading popup (Sort / Files / Settings). Owns its own selection
  // state, level, and chrome — LibraryActivity routes button presses to it
  // and dispatches LeafActivated / SubItemActivated results.
  CascadingPopupMenu popup_;

  // Drives Up/Down hold-to-page-jump (every 500ms after a 500ms hold) and
  // suppresses the tap callback when a continuous fired. Default 500/500
  // matches the cadence used by every settings list in the app.
  ButtonNavigator buttonNavigator;

  // True when the Confirm release that brought us into this activity should
  // not also trigger an open-book (typical when launched from a parent menu).
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Fixed-capacity cover cache sized to three pages worth of thumbs
  // (previous / current / next). The current page is populated lazily on
  // the render path; the two neighbor pages are populated by an async
  // prefetch task so a page-turn paints from cached pixels with no SD
  // I/O on the render thread. The 2bpp decoded source is freed inside
  // buildScaledBitmap once the 1bpp raster is built, keeping the worst-
  // case resident set around 55 KB even at 27 entries (~2 KB / cover).
  BitmapCacheManager pageCache_{PER_PAGE * 3};

  // ---- Prefetch worker state ---------------------------------------------
  // FreeRTOS task that decodes neighbor-page covers off the render path.
  // Created in onEnter, joined and destroyed in onExit. Communication:
  //   - cacheLock_ guards pageCache_, prefetchQueue_, and the queue count.
  //   - prefetchSignal_ is a binary semaphore the worker waits on.
  //   - prefetchCancel_ is a volatile flag the worker checks between
  //     covers to abandon its current scan (set on page change / sort /
  //     rapid-jump / shutdown).
  //   - prefetchShutdown_ tells the worker to exit cleanly.
  // Owner: activity thread enqueues page indices; worker dequeues and
  // populates the cache via GfxRenderer::decodeBitmapEntry + cache.set.
  TaskHandle_t prefetchTask_ = nullptr;
  SemaphoreHandle_t cacheLock_ = nullptr;
  SemaphoreHandle_t prefetchSignal_ = nullptr;
  // Given by the worker after its main loop has exited. onExit waits on
  // this before calling vTaskDelete, ensuring the worker isn't holding
  // cacheLock_ or mid-SD-decode when it's deleted.
  SemaphoreHandle_t prefetchExited_ = nullptr;
  // Given by the worker after each fully-completed page batch (not on
  // cancel). Activity loop polls it to clear lazyLoadCurrentPage_ and
  // trigger a repaint once the awaited page lands in the cache.
  SemaphoreHandle_t prefetchBatchDone_ = nullptr;
  // Capacity 4 is enough for the worst overlap: onEnter enqueues both
  // neighbors (2), and a rapid-jump release enqueues both new neighbors
  // before the worker has drained (2 more). Sentinel 0xFF = empty slot.
  static constexpr std::size_t PREFETCH_QUEUE_CAPACITY = 4;
  uint8_t prefetchQueue_[PREFETCH_QUEUE_CAPACITY] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t prefetchQueueCount_ = 0;
  volatile bool prefetchCancel_ = false;
  volatile bool prefetchShutdown_ = false;

  // Tracks gridHelper.currentPage() at the start of loop() so page
  // transitions trigger the page-aware eviction + direction-of-travel
  // prefetch exactly once per transition.
  uint8_t lastObservedPage_ = 0;
  // True while a rapid-jump (continuous Up/Down) hold is in progress.
  // Render path swaps to peekCachedBitmapDimensions while this is set so
  // pages flicked past don't trigger SD reads; release restores normal
  // loading and triggers a re-prefetch of the landed page's neighbors.
  // Volatile because the render task reads it without taking cacheLock_
  // (the read is single-byte and outside the lock-protected cache ops).
  volatile bool rapidJumping_ = false;
  // True after applySort or endRapidJumpIfActive — render path uses
  // peekCachedBitmapDimensions for the current page so the activity
  // paints placeholders immediately rather than blocking on a 1–2 s
  // synchronous SD decode. Cleared by the activity loop when the
  // prefetch worker signals batch-done (the current page lands first
  // because applySort / endRapidJumpIfActive enqueue it first).
  volatile bool lazyLoadCurrentPage_ = false;

  // ---- Prefetch helpers --------------------------------------------------
  static void prefetchTaskTrampoline(void* ctx);
  void prefetchTaskLoop();
  // Build the SD path for the cover at (page, slot). Returns false if no
  // book occupies that grid position (past end of library). out must be
  // at least 64 bytes.
  bool buildThumbPath(uint8_t page, uint8_t slot, char* out, std::size_t outSize) const;
  // Enqueue a page for prefetch. Validates the page index, dedupes against
  // the existing queue, signals the worker. Safe to call while holding
  // cacheLock_ (uses no-op fast path) or without.
  void enqueuePrefetchLocked(uint8_t page);
  // Cancel any in-flight or queued prefetch. Acquires cacheLock_.
  void cancelAllPrefetch();
  // Drop cached entries whose path doesn't appear in the keep-set for
  // pages {centerPage-1, centerPage, centerPage+1}. Caller holds cacheLock_.
  void evictPagesOutsideKeepSetLocked(uint8_t centerPage);
  // Handle a detected page transition: cancel pending prefetch, evict
  // pages outside the new keep-set, request prefetch for the new
  // direction-of-travel neighbor (or both neighbors on a multi-page jump
  // where the prev page is no longer resident).
  void onPageChanged(uint8_t oldPage, uint8_t newPage);
  // Shared "we just landed on a cold page" reseed used by applySort and
  // endRapidJumpIfActive: cancel any in-flight prefetch, evict entries
  // outside the new {prev, cur, next} keep-set, enqueue current first
  // then both neighbors, drain any stale batch-done signal, enter
  // lazy-load mode so the render path uses peek (showing placeholders)
  // until the worker's first batch lands.
  void primeNeighborhoodLazy(uint8_t centerPage);

  void initPopup();

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
  // Dispatches the activity action for an active popup row (leaf or submenu
  // item). Reads popup_.topSelectedIndex() / subSelectedIndex() to decide.
  void dispatchPopupActivation(CascadingPopupMenu::Nav navResult);
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
  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderBattery(const Rect& headerBox);
  std::string getHeaderSubtitleText();
  void renderLibraryShelf(const Rect& shelfArea);
  void renderPageRail(const Rect& railArea);
  void renderPopup();
  void renderBookTile(const Rect& cell, const LibraryBook& book, bool selected);
  void renderEmptyState(const Rect& body);

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
};
