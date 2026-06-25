#pragma once
#include <BitmapCacheManager.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <string>
#include <vector>

#include "CoverPrefetcher.h"
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

  // Cover cache + async prefetch worker. Fed a flat item index → cover pathHash by
  // the resolver closure below (which adapts the grid view model). Created in
  // onEnter (start), joined/freed in onExit (stop). Hold prefetcher_.lockCache()
  // around cache reads in the render path AND around any rebuildView/sortBy (the
  // worker reads view_ via the resolver under that same lock).
  CoverPrefetcher prefetcher_{renderer, PER_PAGE, [this](int itemIndex) -> std::optional<uint32_t> {
                                const LibraryBook* b = bookForGridIndex(itemIndex);  // null = back tile / OOB
                                return b ? std::optional<uint32_t>(b->pathHash) : std::nullopt;
                              }};

  // True while a rapid-jump (continuous Up/Down) hold is in progress. We don't
  // decode pages flicked past, so the render path peeks and shows placeholders
  // for the uncached page; release (endRapidJumpIfActive) loads the landed page
  // for real. Volatile because the render task reads it without the cache lock.
  volatile bool rapidJumping_ = false;

  // ---- Navigation helpers ------------------------------------------------
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
  // Re-sort the index from current settings and reset selection to the top.
  void applySort();
  // Persist the active sort field/direction and re-sort.
  void setSort(uint8_t field, uint8_t direction);

  // ---- Rendering helpers --------------------------------------------------
  // Compute the cover-slot rect for a grid cell by replaying the same flex
  // layout the render path uses (no drawing). Cells are uniform, so this is the
  // draw envelope for every cover, handed to prefetcher_ (start / setCoverBox) so it
  // scales covers to the exact dims the render path requests.
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
  AppId appId() const override { return AppId::Library; }
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    return GlobalMenuConfig{.clearFontCacheOnClose = false};
  }
};
