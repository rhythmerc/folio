#pragma once
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "LibraryIndex.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect

class LibraryActivity final : public Activity {
 public:
  // Toggled by the Menu button. The library is the default view; Menu replaces
  // the content area with a three-item list (Browse Files, File Transfer,
  // Settings).
  enum class View { Library, Menu };

 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;
  static constexpr int MENU_ITEMS = 3;

  // ---- View state ---------------------------------------------------------
  View view = View::Library;

  // Library grid position
  int libraryPage = 0;          // 0-indexed
  int librarySelected = 0;      // 0..PER_PAGE-1 within current page

  // Menu position
  int menuSelected = 0;         // 0..MENU_ITEMS-1

  // True when the Confirm release that brought us into this activity should
  // not also trigger an open-book (typical when launched from a parent menu).
  bool lockNextConfirmRelease = false;

  // Per-slot framebuffer-region cache for cover bitmaps. Cover pixels don't
  // depend on selection state (the selection hatch lives outside the cover
  // rect, and we fillRect-clear the cover area before drawing the BMP), so
  // we can capture each cover once per page and memcpy it back on every
  // subsequent render instead of re-opening + re-decoding the .bmp file
  // from SD. Modeled on HomeActivity's coverBuffer pattern.
  struct CoverCache {
    std::unique_ptr<uint8_t[]> buf;
    size_t size = 0;
    bool valid = false;
  };
  CoverCache coverCache[PER_PAGE];
  int cachedCoverPage = -1;

  // ---- Navigation helpers -------------------------------------------------
  int currentRow() const { return librarySelected / COLS; }
  int currentCol() const { return librarySelected % COLS; }
  int totalPages() const { return LIBRARY_INDEX.totalPages(PER_PAGE); }

  void moveUp();
  void moveDown();
  void moveLeft();
  void moveRight();
  void doSelect();
  void toggleMenu();
  void openMenuOption(int idx);

  // ---- Rendering helpers --------------------------------------------------
  // Batch-prewarms every SD-loaded role font with the exact text it'll
  // render this frame. Without this, the first draw of each glyph faults
  // it from the SD card one byte at a time (~14 s per selection change for
  // a 9-book grid). Called from render() before any draw operations.
  void prewarmFonts();
  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderHeader();
  void renderLibraryShelf();
  void renderPageRail();
  void renderMenuView();
  void renderBookTile(int slotIndex, const LibraryBook& book, bool selected);
  void renderEmptyState();

  // Returns the rect actually occupied by a book tile's content (cover +
  // title + author + progress bar) inside its cell. Shorter than the cell
  // itself when the title fits on one line or there's no author / progress.
  // Used by the selection frame so it hugs the visible content the way the
  // prototype's CSS outline does.
  Rect tileContentRect(const LibraryBook& book, const Rect& cell) const;

  // Cover-cache helpers. invalidateCoverCache() drops all slot buffers and
  // marks cachedCoverPage stale; called on page change and onExit.
  void invalidateCoverCache();

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
};
