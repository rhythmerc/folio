#pragma once
#include <I18n.h>

#include <cstdint>
#include <string>

#include "LibraryIndex.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

class LibraryActivity final : public Activity {
 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;

  // Top-panel row indices for the cascading popup. The activity dispatches
  // leaf actions and sort logic against these indices.
  static constexpr int POPUP_TOP_SORT = 0;
  static constexpr int POPUP_TOP_FILES = 1;
  static constexpr int POPUP_TOP_SETTINGS = 2;
  static constexpr int POPUP_TOP_COUNT = 3;

  static constexpr int POPUP_FILES_BROWSE = 0;
  static constexpr int POPUP_FILES_TRANSFER = 1;
  static constexpr int POPUP_FILES_COUNT = 2;

  static constexpr int POPUP_SORT_COUNT = 4;  // Recent / Title / Author / Progress

  // ---- View state ---------------------------------------------------------
  // Library grid position
  int libraryPage = 0;          // 0-indexed
  int librarySelected = 0;      // 0..PER_PAGE-1 within current page

  // The cascading popup (Sort / Files / Settings). Owns its own selection
  // state, level, and chrome — LibraryActivity routes button presses to it
  // and dispatches LeafActivated / SubItemActivated results.
  CascadingPopupMenu popup_;

  // True when the Confirm release that brought us into this activity should
  // not also trigger an open-book (typical when launched from a parent menu).
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // No per-slot cover cache here — the renderer owns a path-keyed image
  // cache (see GfxRenderer::drawCachedBitmap). Library just calls
  // drawCachedBitmap for each visible thumb; the renderer hides the
  // SD-I/O / decode cost and LRU-evicts under its own budget.

  // ---- Navigation helpers -------------------------------------------------
  int currentRow() const { return librarySelected / COLS; }
  int currentCol() const { return librarySelected % COLS; }
  int totalPages() const { return LIBRARY_INDEX.totalPages(PER_PAGE); }

  void initPopup();

  void moveUp();
  void moveDown();
  void moveLeft();
  void moveRight();
  void doSelect();
  // Dispatches the activity action for an active popup row (leaf or submenu
  // item). Reads popup_.topSelectedIndex() / subSelectedIndex() to decide.
  void dispatchPopupActivation(CascadingPopupMenu::Nav navResult);
  // Re-sort the index from current settings and reset selection to the top.
  void applySort();
  // Persist the active sort field/direction and re-sort.
  void setSort(uint8_t field, uint8_t direction);

  // ---- Rendering helpers --------------------------------------------------
  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderHeader();
  void renderLibraryShelf();
  void renderPageRail();
  void renderPopup();
  void renderBookTile(int slotIndex, const LibraryBook& book, bool selected);
  void renderEmptyState();

  // Returns the rect actually occupied by a book tile's content (cover +
  // title + author + progress bar) inside its cell. Shorter than the cell
  // itself when the title fits on one line or there's no author / progress.
  // Used by the selection frame so it hugs the visible content the way the
  // prototype's CSS outline does.
  Rect tileContentRect(const LibraryBook& book, const Rect& cell) const;


  // Bounding rect of the (row, col) cell inside the shelf area. Pure geometry,
  // marked static so the renderer code keeps direct access to COLS/ROWS without
  // a `this` indirection.
  static Rect cellRect(int row, int col, int shelfX, int shelfY, int shelfW, int shelfH);

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void declareText(TextCollector& tc) override;

  // Power-button override: short-press advances selection rightward in the
  // book grid (mirrors the front Right button). The side-rail hint label
  // is set inline at the renderSide call site.
  bool handlePowerShortPress() override;
};
