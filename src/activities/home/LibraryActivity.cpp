#include "LibraryActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "FontCacheManager.h"
#include "MappedInputManager.h"
#include "ThemeFontRegistry.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.h"
#include "fontIds.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

// Resolve font roles against the currently-active theme so Library follows
// whichever theme the user has selected. Each theme provides its own role
// mapping (and SD-installed face, if any) via ThemeData; Library now picks
// up sans-serif Lyra fonts under Lyra, Folio serif under Folio, etc.
int libFont(FontRole role) { return GUI.getFontForRole(role); }

// Long-press threshold for Back-to-Home, matching the firmware convention.
constexpr unsigned long GO_HOME_MS = 1000;

// Layout (in physical-pixel coordinates, portrait orientation 480 × 800).
// Header is matched to the prototype: 89 px tall with a 3 px inner border.
constexpr int HEADER_HEIGHT = 89;
constexpr int HEADER_BOTTOM_BORDER = 3;
constexpr int FOOTER_HEIGHT = 40;

constexpr int CONTENT_PAD_X = 18;
constexpr int CONTENT_PAD_Y = 8;
constexpr int RAIL_WIDTH = 18;
constexpr int RAIL_GAP = 10;

// Book tile geometry. Tile height is fixed (not adaptive to content) so the
// selection frame, drop shadow, and inter-tile spacing read the same for
// every book — whether the title fits on one line or two, whether the
// author is set, whether the book has progress, etc. Title + author are
// then centered vertically inside the reserved text area.
constexpr int COVER_W = 80;
constexpr int COVER_H = 120;
constexpr int CELL_GAP = 14;
constexpr int TILE_PAD_TOP = 4;
constexpr int TILE_TEXT_AREA_H = 60;             // reserved for title + author, regardless of content
constexpr int TILE_TITLE_AUTHOR_GAP = 1;         // gap between title's last line and author
constexpr int TILE_PROGRESS_MARGIN_TOP = 3;
constexpr int TILE_PROGRESS_HEIGHT = 4;          // inner fill height
constexpr int TILE_PROGRESS_TOTAL_H = TILE_PROGRESS_HEIGHT + 2;  // + 2 for 1-px border each side
// Total fixed tile content height — same for every book.
constexpr int TILE_CONTENT_H = COVER_H + TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP + TILE_PROGRESS_TOTAL_H;

// Page rail tick geometry.
constexpr int RAIL_TICK = 9;
constexpr int RAIL_TICK_GAP = 10;
constexpr int RAIL_PAD_TOP = 8;
constexpr int RAIL_TICK_COUNT = 6;   // visual cap on the rail (extras roll off)

}  // namespace

Rect LibraryActivity::cellRect(int row, int col, int shelfX, int shelfY, int shelfW, int shelfH) {
  const int cellW = (shelfW - CELL_GAP * (COLS - 1)) / COLS;
  const int cellH = (shelfH - CELL_GAP * (ROWS - 1)) / ROWS;
  return Rect{shelfX + col * (cellW + CELL_GAP), shelfY + row * (cellH + CELL_GAP), cellW, cellH};
}

// ---- Lifecycle --------------------------------------------------------------

void LibraryActivity::onEnter() {
  Activity::onEnter();

  // Ignore the Confirm release that brought us here (otherwise we'd
  // immediately auto-open the first book).
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  view = View::Library;
  menuSelected = 0;

  // SD role fonts persist across most Library exits — only the book-open
  // path (doSelect → goToReader) unloads them. When returning from the
  // reader, reloadActive re-scans the active theme's directory. The first
  // render after this will call declareText, which batched-loads the
  // glyphs we actually need into the LRU.
  THEME_FONTS.reloadActive(renderer);

  // The on-disk library.bin survives onExit; load it back into memory.
  LIBRARY_INDEX.loadFromFile();

  // Always do a refresh pass — picks up newly added EPUBs, drops removed ones,
  // and refreshes progress. New EPUBs trigger a blocking "Indexing library..."
  // popup; the popup never appears when nothing has changed.
  LIBRARY_INDEX.refreshFromSdCard(&renderer);

  // Clamp selection in case the previous page no longer exists.
  const int pages = totalPages();
  if (libraryPage >= pages) libraryPage = std::max(0, pages - 1);
  if (LIBRARY_INDEX.getAt(libraryPage, librarySelected, PER_PAGE) == nullptr) {
    librarySelected = 0;
  }

  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();

  // Free Library-only state that other activities don't need. SD role fonts
  // are NOT unloaded here — that would silently drop typography in every
  // subsequent UI activity (Settings, Browse, etc. would lose the Folio
  // serif and fall back to embedded sans). The book-open path in doSelect()
  // does its own font unload before goToReader, which is the one exit that
  // actually needs the RAM.
  //
  // The renderer's image cache holds 9 decoded covers (~10 KB total at 1bpp).
  // We drop those on exit too — the next paint of any other UI is fast
  // because those activities don't render thumbnails.
  renderer.clearImageCache();
  LIBRARY_INDEX.unload();
}

// ---- Input ------------------------------------------------------------------

void LibraryActivity::loop() {
  // Suppress the just-pressed Confirm release that brought us here.
  if (lockNextConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      lockNextConfirmRelease = false;
    }
    return;
  }

  // Long-press Back → exit to home regardless of view.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Menu) {
      view = View::Library;
      requestUpdate();
    } else {
      // From Library, single-tap Back opens the Menu (matches the
      // prototype's "Menu" footer label = the Back front button).
      toggleMenu();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSelect();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    moveUp();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    moveDown();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    moveLeft();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    moveRight();
    return;
  }
}

// ---- Navigation -------------------------------------------------------------

void LibraryActivity::moveUp() {
  if (view == View::Menu) {
    if (menuSelected > 0) {
      menuSelected--;
      requestUpdate();
    }
    return;
  }
  const int row = currentRow();
  const int col = currentCol();
  if (row > 0) {
    librarySelected = (row - 1) * COLS + col;
    requestUpdate();
  } else if (libraryPage > 0) {
    libraryPage--;
    librarySelected = (ROWS - 1) * COLS + col;
    requestUpdate();
  }
}

void LibraryActivity::moveDown() {
  if (view == View::Menu) {
    if (menuSelected < MENU_ITEMS - 1) {
      menuSelected++;
      requestUpdate();
    }
    return;
  }
  const int row = currentRow();
  const int col = currentCol();

  // Target the row below on the current page; if we're on the last row,
  // advance to row 0 of the next page (if any).
  int targetPage = libraryPage;
  int targetRow;
  if (row < ROWS - 1) {
    targetRow = row + 1;
  } else if (libraryPage < totalPages() - 1) {
    targetPage = libraryPage + 1;
    targetRow = 0;
  } else {
    return;
  }

  // Clamp left until we find a filled slot. The last page may be partial,
  // so col can land on an empty cell — without this clamp the selection
  // ends up pointing at a hole (visually nothing selected).
  for (int c = col; c >= 0; --c) {
    if (LIBRARY_INDEX.getAt(targetPage, targetRow * COLS + c, PER_PAGE) != nullptr) {
      if (targetPage != libraryPage) {
        libraryPage = targetPage;
      }
      librarySelected = targetRow * COLS + c;
      requestUpdate();
      return;
    }
  }
}

void LibraryActivity::moveLeft() {
  if (view == View::Menu) return;  // Left/Right are no-ops in the menu.
  const int col = currentCol();
  if (col > 0) {
    librarySelected--;
    requestUpdate();
  }
}

void LibraryActivity::moveRight() {
  if (view == View::Menu) return;
  const int col = currentCol();
  if (col < COLS - 1) {
    const int candidate = librarySelected + 1;
    if (LIBRARY_INDEX.getAt(libraryPage, candidate, PER_PAGE) != nullptr) {
      librarySelected = candidate;
      requestUpdate();
    }
  }
}

void LibraryActivity::doSelect() {
  if (view == View::Menu) {
    openMenuOption(menuSelected);
    return;
  }
  const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, librarySelected, PER_PAGE);
  if (book == nullptr) return;
  LOG_DBG(LOG_TAG, "Opening book: %s", book->path.c_str());

  // The reader is the RAM-critical destination — EPUB indexing wants every
  // byte it can get. Free SD role fonts (~50 KB persistent + ~30 KB mini
  // cache) before handing off. Library's onEnter re-discovers + re-prewarms
  // when we come back. Other Library exits (Settings, Browse, Home) keep
  // the fonts resident so those activities stay typographically consistent
  // and don't lazy-fault glyphs on first paint.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();
  THEME_FONTS.unloadAll(renderer);

  activityManager.goToReader(book->path);
}

void LibraryActivity::toggleMenu() {
  view = (view == View::Menu) ? View::Library : View::Menu;
  if (view == View::Menu) menuSelected = 0;
  requestUpdate();
}

void LibraryActivity::openMenuOption(int idx) {
  switch (idx) {
    case 0:
      activityManager.goToFileBrowser();
      break;
    case 1:
      activityManager.goToFileTransfer();
      break;
    case 2:
      activityManager.goToSettings();
      break;
    default:
      break;
  }
}

// ---- Render -----------------------------------------------------------------

void LibraryActivity::render(RenderLock&&) {
  // Single-pass render. ActivityManager called declareText() just before
  // this, so the font cache LRU already holds the glyphs this paint needs.
  // Anything the declaration missed (rare punctuation in a book title,
  // CJK, etc.) lazy-loads into the LRU via SdCardFont::onGlyphMiss — those
  // entries also persist across paints, so a one-off slow first paint is
  // the worst case for unforeseen glyphs.
  renderer.clearScreen();
  renderPasses();
  renderer.displayBuffer();
}

void LibraryActivity::declareText(TextCollector& tc) {
  // Enumerate exactly the text this paint will draw. The framework
  // batched-loads any glyphs we haven't cached yet; SdCardFont's idempotent
  // prewarm short-circuits when the same content was declared last paint
  // (i.e. selection movement on the same shelf page).
  //
  // Library uses Compact variants for the dense book-grid text (titles,
  // authors, page rail) and default sizes for header/menu chrome — the
  // user installs separate compact + default .cpfont files for the
  // contrast. When only one is installed, the Compact lookup falls through
  // to the default font so the screen still renders.
  const int titleFont = libFont(FontRole::Title);
  const int headingFont = libFont(FontRole::Heading);
  const int bodyCompactFont = libFont(FontRole::BodyCompact);
  const int captionFont = libFont(FontRole::Caption);
  const int captionCompactFont = libFont(FontRole::CaptionCompact);
  const int accentCompactFont = libFont(FontRole::AccentCompact);

  if (view == View::Menu) {
    // Menu view chrome — default sizes, this view is roomy.
    tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_LIBRARY_MENU_TITLE));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_MENU_SUBTITLE));
    tc.use(headingFont, EpdFontFamily::ITALIC, "I.II.III.");
    tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_BROWSE_FILES));
    tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_FILE_TRANSFER));
    tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_SETTINGS_TITLE));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_MENU_BROWSE_HINT));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_MENU_TRANSFER_HINT));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_MENU_SETTINGS_HINT));
  } else {
    // Library shelf view.
    // Header — default caption (header subtitle is short, doesn't need compact).
    tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_LIBRARY));
    if (!LIBRARY_INDEX.isEmpty()) {
      tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_SORTED_RECENT));
      tc.use(captionFont, EpdFontFamily::ITALIC, "0123456789 · /");
      // Page rail "1 / 12" — uses the compact accent face.
      tc.use(accentCompactFont, EpdFontFamily::ITALIC, "0123456789 /");
    }
    // Book grid — dense, uses the compact caption face.
    for (int slot = 0; slot < PER_PAGE; ++slot) {
      const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, slot, PER_PAGE);
      if (book == nullptr) continue;
      tc.use(captionCompactFont, EpdFontFamily::BOLD, book->title);
      if (!book->author.empty()) {
        tc.use(captionCompactFont, EpdFontFamily::ITALIC, book->author);
      }
    }
  }

  // Button hints — italic body labels, compact face so the label has
  // breathing room inside the 106-px Folio hint box.
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_MENU_LABEL));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_BACK));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_SELECT));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_DIR_LEFT));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_DIR_RIGHT));
}

void LibraryActivity::renderPasses() {
  renderHeader();
  if (view == View::Library) {
    if (LIBRARY_INDEX.isEmpty()) {
      renderEmptyState();
    } else {
      renderLibraryShelf();
      renderPageRail();
    }
  } else {
    renderMenuView();
  }

  // Footer button hints — delegate to the active theme so they match the
  // rest of the device's chrome. Folio paints hairline-italic hints, Lyra
  // paints rounded white-filled hints, Classic paints sharp boxes, etc.
  const char* backLabel = (view == View::Menu) ? tr(STR_BACK) : tr(STR_MENU_LABEL);
  const char* leftLabel = (view == View::Menu) ? "" : tr(STR_DIR_LEFT);
  const char* rightLabel = (view == View::Menu) ? "" : tr(STR_DIR_RIGHT);
  const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LibraryActivity::renderHeader() {
  // 3px inner bottom border at the bottom of the 89-px header band.
  renderer.fillRect(0, HEADER_HEIGHT - HEADER_BOTTOM_BORDER, renderer.getScreenWidth(), HEADER_BOTTOM_BORDER);

  // Battery icon top-right. Delegates to whichever theme is active —
  // intentional, the battery is one of the few elements LibraryActivity
  // happily inherits from the user's chosen theme.
  const auto& td = *GUI.getData();
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(renderer.getScreenWidth() - maxBatteryWidth, 5, maxBatteryWidth, td.battery.height + 10, false);
  const bool showBatteryPct =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = renderer.getScreenWidth() - 12 - td.battery.width;
  GUI.drawBatteryRight(renderer, Rect{batteryX, 5, td.battery.width, td.battery.height}, showBatteryPct);

  // Title and subtitle — match FolioTheme::drawHeader's positions so the
  // Library reads identically whether or not Folio is the active theme.
  const bool inMenu = (view == View::Menu);
  const char* title = inMenu ? tr(STR_LIBRARY_MENU_TITLE) : tr(STR_LIBRARY);

  std::string subtitleText;
  if (inMenu) {
    subtitleText = tr(STR_LIBRARY_MENU_SUBTITLE);
  } else if (!LIBRARY_INDEX.isEmpty()) {
    subtitleText = std::string(tr(STR_LIBRARY_SORTED_RECENT)) + "  ·  " +
                   std::to_string(libraryPage + 1) + " / " + std::to_string(totalPages());
  }

  const int titleFont = libFont(FontRole::Title);
  const int captionFont = libFont(FontRole::Caption);

  const int titleMaxWidth = batteryX - CONTENT_PAD_X * 2;
  const std::string truncatedTitle =
      renderer.truncatedText(titleFont, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(titleFont, CONTENT_PAD_X, 20, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);

  if (!subtitleText.empty()) {
    const std::string truncatedSub = renderer.truncatedText(
        captionFont, subtitleText.c_str(), renderer.getScreenWidth() - CONTENT_PAD_X * 2, EpdFontFamily::ITALIC);
    renderer.drawText(captionFont, CONTENT_PAD_X, 56, truncatedSub.c_str(), true, EpdFontFamily::ITALIC);
  }
}

void LibraryActivity::renderLibraryShelf() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  const int shelfX = CONTENT_PAD_X;
  const int shelfY = HEADER_HEIGHT + CONTENT_PAD_Y;
  const int shelfW = screenW - CONTENT_PAD_X * 2 - RAIL_WIDTH - RAIL_GAP;
  const int shelfH = screenH - HEADER_HEIGHT - FOOTER_HEIGHT - CONTENT_PAD_Y * 2;

  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, slot, PER_PAGE);
    if (book == nullptr) continue;
    const Rect cell = cellRect(slot / COLS, slot % COLS, shelfX, shelfY, shelfW, shelfH);
    const bool selected = (slot == librarySelected);

    // Selection is split into a background pass (fills — drawn BEFORE tile
    // content so the cover bitmap and text paint on top of the wash) and a
    // foreground pass (borders + brackets — drawn AFTER content so the
    // frame sits on top). Themes with only fills (Lyra's RoundedFill) are
    // no-op in foreground; themes with only borders (Folio's LayeredFrame)
    // are no-op in background.
    //
    // Horizontal inset hugs the content (3 px clears Folio's layered outer
    // + gap + inner stroke). Vertical inset is deliberately larger so the
    // frame "lifts" the tile with breathing room above and below instead
    // of clinging tightly to the cover/text bounds.
    constexpr int kFrameInsetX = 3;
    constexpr int kFrameInsetY = 8;
    Rect selectionRect{};
    if (selected) {
      const Rect content = tileContentRect(*book, cell);
      selectionRect = Rect{content.x - kFrameInsetX, content.y - kFrameInsetY,
                           content.width + kFrameInsetX * 2, content.height + kFrameInsetY * 2};
      GUI.drawSelectionBackground(renderer, selectionRect);
    }
    renderBookTile(slot, *book, selected);
    if (selected) {
      GUI.drawSelectionForeground(renderer, selectionRect);
    }
  }
}

Rect LibraryActivity::tileContentRect(const LibraryBook& /*book*/, const Rect& cell) const {
  // Fixed-height tile: every card occupies the same vertical extent
  // regardless of title length, presence of author, or read state. Makes
  // the selection frame and inter-tile spacing read uniformly across the
  // shelf. Title + author get centered vertically inside the reserved
  // text area in renderBookTile.
  return Rect{cell.x, cell.y + TILE_PAD_TOP, cell.width, TILE_CONTENT_H};
}

void LibraryActivity::renderBookTile(int slotIndex, const LibraryBook& book, bool selected) {
  // When the active theme paints a dark selection background (Classic's
  // SolidFill, RoundedRaff's RoundedRowAlways), the tile's text + progress
  // bar need to render in inverted (white) ink to stay legible. The cover
  // bitmap is unaffected — renderBookTile fills the cover area to white
  // before drawing the BMP, so the cover always sits on a white substrate
  // regardless of selection background.
  const bool invertText = selected && GUI.getData()->selection.textInverted;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int shelfX = CONTENT_PAD_X;
  const int shelfY = HEADER_HEIGHT + CONTENT_PAD_Y;
  const int shelfW = screenW - CONTENT_PAD_X * 2 - RAIL_WIDTH - RAIL_GAP;
  const int shelfH = screenH - HEADER_HEIGHT - FOOTER_HEIGHT - CONTENT_PAD_Y * 2;
  const Rect cell = cellRect(slotIndex / COLS, slotIndex % COLS, shelfX, shelfY, shelfW, shelfH);

  // Resolve fonts up front. The 3×3 grid is the densest screen the device
  // renders, so we use the *Compact* caption — when the user installs
  // /.fonts/themes/folio/caption-compact.cpfont at a smaller point size,
  // Library tightens up while Settings / Browse keep the larger default
  // caption. Falls through to the regular caption when no compact face is
  // installed, so this works the day someone first installs the theme too.
  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  // ---- Cover (centered horizontally within the cell, top-aligned) -----
  // The renderer's path-keyed image cache owns the SD-I/O + decode work.
  // First paint of a thumb loads from SD; subsequent paints rasterize
  // from cached pixel data. Library just queries dimensions, computes
  // the centered frame, and calls drawCachedBitmap.
  //
  // The "cover area" is the full cell width × COVER_H — lets wider
  // thumbnails (square or near-square sources) keep their natural size
  // instead of being clipped to a fixed 80-px slot.
  const int coverAreaX = cell.x;
  const int coverAreaW = cell.width;
  const int coverY = cell.y + TILE_PAD_TOP;

  // Default frame for the title-only fallback (no thumb on disk).
  int frameX = cell.x + (cell.width - COVER_W) / 2;
  int frameY = coverY;
  int frameW = COVER_W;
  int frameH = COVER_H;

  bool drewCover = false;
  const std::string thumbPath =
      std::string("/.crosspoint/epub_") + std::to_string(book.pathHash) + "/thumb_144.bmp";
  int bmpW = 0, bmpH = 0;
  if (renderer.getCachedBitmapDimensions(thumbPath.c_str(), &bmpW, &bmpH) && bmpW > 0 && bmpH > 0) {
    // Fit-to-box against (cell.width × COVER_H), preserving aspect.
    float scale = 1.0f;
    if (bmpW > coverAreaW) scale = static_cast<float>(coverAreaW) / static_cast<float>(bmpW);
    if (bmpH > COVER_H) scale = std::min(scale, static_cast<float>(COVER_H) / static_cast<float>(bmpH));
    const int drawnW = static_cast<int>(bmpW * scale);
    const int drawnH = static_cast<int>(bmpH * scale);
    // Center within the cell horizontally and within the cover height
    // vertically. Tall covers naturally bottom-align since drawnH hits
    // COVER_H first.
    frameX = coverAreaX + (coverAreaW - drawnW) / 2;
    frameY = coverY + (COVER_H - drawnH) / 2;
    frameW = drawnW;
    frameH = drawnH;
    // Clear just the bitmap rect to white. drawCachedBitmap only writes
    // black pixels; the white substrate keeps the selection background
    // (Lyra / Classic / RoundedRaff) showing in the slot margins around
    // the cover.
    renderer.fillRect(frameX, frameY, frameW, frameH, false);
    drewCover = renderer.drawCachedBitmap(thumbPath.c_str(), frameX, frameY, frameW, frameH);
  }
  if (!drewCover) {
    // Fallback: title-only "cover" — outlined rectangle with the first
    // line of the title centered inside.
    renderer.fillRect(frameX, frameY, frameW, frameH, false);
    const std::string trunc =
        renderer.truncatedText(captionFont, book.title.c_str(), COVER_W - 12, EpdFontFamily::BOLD);
    const int tw = renderer.getTextWidth(captionFont, trunc.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(captionFont, frameX + (COVER_W - tw) / 2, frameY + (COVER_H - captionLineH) / 2,
                      trunc.c_str(), true, EpdFontFamily::BOLD);
  }
  // 1 px outer border + 1 px offset drop shadow on the cover (Folio motif).
  renderer.drawRect(frameX, frameY, frameW, frameH);
  renderer.drawLine(frameX + 1, frameY + frameH, frameX + frameW, frameY + frameH);
  renderer.drawLine(frameX + frameW, frameY + 1, frameX + frameW, frameY + frameH);

  // ---- Text area (fixed-height, title + author centered vertically) -----
  // Card height is fixed (see TILE_CONTENT_H), so the title + author block
  // gets centered inside the reserved space immediately below the cover.
  // Books with single-line titles or no author still occupy the same total
  // vertical extent — the leftover space becomes balanced padding above
  // and below the block.
  //
  // When the book has no progress bar, the reserved progress area (margin
  // + bar) becomes part of the centering range — otherwise unread books
  // look top-heavy with the progress slot sitting empty below.
  const int textAreaY = coverY + COVER_H;
  const int centeringH = book.hasProgress()
                             ? TILE_TEXT_AREA_H
                             : (TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP + TILE_PROGRESS_TOTAL_H);

  // Decide how many title lines fit. Author always reserves its line of
  // space when present, so the title gets whatever's left.
  const int authorReserved = book.author.empty() ? 0 : (TILE_TITLE_AUTHOR_GAP + captionLineH);
  const int titleBudget = centeringH - authorReserved;
  const int maxTitleLines = std::min(2, std::max(1, titleBudget / captionLineH));
  std::vector<std::string> titleLines =
      renderer.wrappedText(captionFont, book.title.c_str(), cell.width - 8, maxTitleLines, EpdFontFamily::BOLD);
  const int titleLineCount = std::max(1, static_cast<int>(titleLines.size()));

  const int titleH = titleLineCount * captionLineH;
  const int blockH = titleH + authorReserved;
  const int blockTop = textAreaY + (centeringH - blockH) / 2;

  // Title — centered horizontally per line, stacked vertically from blockTop.
  // Text color flips when the theme's selection paints a dark background.
  const bool textBlack = !invertText;
  for (size_t i = 0; i < titleLines.size(); ++i) {
    const int lineW = renderer.getTextWidth(captionFont, titleLines[i].c_str(), EpdFontFamily::BOLD);
    renderer.drawText(captionFont, cell.x + (cell.width - lineW) / 2,
                      blockTop + static_cast<int>(i) * captionLineH, titleLines[i].c_str(), textBlack,
                      EpdFontFamily::BOLD);
  }

  // Author — italic, single line, directly below the title's last line.
  if (!book.author.empty()) {
    const int authorY = blockTop + titleH + TILE_TITLE_AUTHOR_GAP;
    const std::string author =
        renderer.truncatedText(captionFont, book.author.c_str(), cell.width - 8, EpdFontFamily::ITALIC);
    const int aw = renderer.getTextWidth(captionFont, author.c_str(), EpdFontFamily::ITALIC);
    renderer.drawText(captionFont, cell.x + (cell.width - aw) / 2, authorY, author.c_str(), textBlack,
                      EpdFontFamily::ITALIC);
  }

  // ---- Progress bar (fixed position, only drawn when book has progress) -
  // Bar Y is anchored at the end of the reserved text area, so every row's
  // bars align horizontally regardless of how each title wrapped. Unread
  // books just leave this area blank — preserving fixed card height.
  if (book.hasProgress()) {
    const int barY = textAreaY + TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP;
    const int barX = cell.x + (cell.width - COVER_W) / 2;
    // Substrate fill in the *opposite* of the ink color so the bar's empty
    // portion always reads as a clean background-of-ink, regardless of
    // what's painted behind the tile. Without this, Lyra's LightGray
    // selection wash shows through the empty part of the bar (noisy
    // dither under the progress indicator) and Classic/RoundedRaff's
    // black selection bg makes the bar's empty area indistinguishable
    // from the surrounding selection.
    renderer.fillRect(barX, barY, COVER_W, TILE_PROGRESS_TOTAL_H, !textBlack);
    renderer.drawRect(barX, barY, COVER_W, TILE_PROGRESS_TOTAL_H, textBlack);
    const int fillW = (COVER_W - 4) * book.progressPercent() / 100;
    if (fillW > 0) {
      renderer.fillRect(barX + 2, barY + 2, fillW, TILE_PROGRESS_HEIGHT - 2, textBlack);
    }
  }
}

void LibraryActivity::renderPageRail() {
  // Always render the rail so the user sees their position even on a single-
  // page library — the visual treatment is part of the Library's identity.
  const int pages = std::max(1, totalPages());
  const int screenH = renderer.getScreenHeight();
  const int railX = renderer.getScreenWidth() - CONTENT_PAD_X - RAIL_WIDTH;
  const int railTop = HEADER_HEIGHT + CONTENT_PAD_Y + RAIL_PAD_TOP;
  const int railBottom = screenH - FOOTER_HEIGHT - CONTENT_PAD_Y;

  const int visibleTicks = std::min(pages, RAIL_TICK_COUNT);
  const int tickX = railX + (RAIL_WIDTH - RAIL_TICK) / 2;
  for (int i = 0; i < visibleTicks; ++i) {
    const int tickY = railTop + i * (RAIL_TICK + RAIL_TICK_GAP);
    if (i == libraryPage) {
      renderer.fillRect(tickX, tickY, RAIL_TICK, RAIL_TICK);
    } else {
      renderer.drawRect(tickX, tickY, RAIL_TICK, RAIL_TICK);
    }
  }

  // Page count at the bottom of the rail, rotated 90° clockwise for that
  // editorial vertical-marginalia feel from the prototype. Uses the
  // Compact accent so the vertical marginalia stays tight against the
  // rail; falls through to the regular accent when no compact face is
  // installed.
  const int accentFont = libFont(FontRole::AccentCompact);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", libraryPage + 1, pages);
  const int countY = railBottom - 4;
  const int countX = railX + RAIL_WIDTH / 2 + 4;
  renderer.drawTextRotated90CW(accentFont, countX, countY, countBuf, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderEmptyState() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int font = libFont(FontRole::Heading);
  const char* msg = tr(STR_LIBRARY_NO_BOOKS);
  const int textW = renderer.getTextWidth(font, msg, EpdFontFamily::ITALIC);
  const int y = HEADER_HEIGHT + (screenH - HEADER_HEIGHT - FOOTER_HEIGHT) / 2;
  renderer.drawText(font, (screenW - textW) / 2, y, msg, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderMenuView() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int areaY = HEADER_HEIGHT + CONTENT_PAD_Y + 22;
  const int areaH = screenH - HEADER_HEIGHT - FOOTER_HEIGHT - CONTENT_PAD_Y * 2 - 44;

  // Three items, evenly distributed in the available area.
  const int gap = 20;
  const int itemH = (areaH - gap * (MENU_ITEMS - 1)) / MENU_ITEMS;
  const int itemX = CONTENT_PAD_X + 12;
  const int itemW = screenW - (CONTENT_PAD_X + 12) * 2;

  const char* romanLabels[MENU_ITEMS] = {"I.", "II.", "III."};
  const char* labels[MENU_ITEMS] = {tr(STR_BROWSE_FILES), tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
  const char* metas[MENU_ITEMS] = {tr(STR_LIBRARY_MENU_BROWSE_HINT), tr(STR_LIBRARY_MENU_TRANSFER_HINT),
                                   tr(STR_LIBRARY_MENU_SETTINGS_HINT)};

  const int titleFont = libFont(FontRole::Title);
  const int headingFont = libFont(FontRole::Heading);
  const int captionFont = libFont(FontRole::Caption);

  const bool invertText = GUI.getData()->selection.textInverted;
  for (int i = 0; i < MENU_ITEMS; ++i) {
    const int y = areaY + i * (itemH + gap);
    const Rect item{itemX, y, itemW, itemH};
    const bool selected = (i == menuSelected);
    if (selected) {
      GUI.drawSelectionBackground(renderer, item);
    }
    const bool textBlack = !(selected && invertText);

    // Numeral (heading-sized italic, dimmer feel than the label).
    const int numW = renderer.getTextWidth(headingFont, romanLabels[i], EpdFontFamily::ITALIC);
    renderer.drawText(headingFont, item.x + 16, item.y + 16, romanLabels[i], textBlack, EpdFontFamily::ITALIC);

    // Label — title-sized bold serif (matches the prototype's 28-px primary).
    const int labelX = item.x + 16 + numW + 14;
    renderer.drawText(titleFont, labelX, item.y + 14, labels[i], textBlack, EpdFontFamily::BOLD);

    // Hairline rule beneath the label (cover-type motif).
    const int ruleY = item.y + 14 + renderer.getLineHeight(titleFont) + 4;
    renderer.drawLine(labelX, ruleY, labelX + 30, ruleY, textBlack);

    // Meta line (caption italic, secondary).
    renderer.drawText(captionFont, labelX, ruleY + 6, metas[i], textBlack, EpdFontFamily::ITALIC);

    if (selected) {
      GUI.drawSelectionForeground(renderer, item);
    }
  }
}
