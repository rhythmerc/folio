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
#include "components/themes/folio/FolioTheme.h"
#include "fontIds.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

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

// Book tile geometry. Cover sized so that the 209-px row track holds the cover
// + 2-line NOTOSERIF_10 title + 1-line NOTOSERIF_10 italic author + 4-px
// progress bar without overflow. NOTOSERIF_10 (smallest embedded serif) is
// taller per line than the prototype's CSS 11–13 px, so the cover gives up
// some height to make room.
constexpr int COVER_W = 80;
constexpr int COVER_H = 120;
constexpr int CELL_GAP = 14;
constexpr int TILE_TITLE_MARGIN_TOP = 4;
constexpr int TILE_AUTHOR_MARGIN_TOP = 1;
constexpr int TILE_PROGRESS_MARGIN_TOP = 3;
constexpr int TILE_PROGRESS_HEIGHT = 4;

// Page rail tick geometry.
constexpr int RAIL_TICK = 9;
constexpr int RAIL_TICK_GAP = 10;
constexpr int RAIL_PAD_TOP = 8;
constexpr int RAIL_TICK_COUNT = 6;   // visual cap on the rail (extras roll off)

// Footer button hints — match Folio's geometry exactly so the labels line up
// with the physical front buttons regardless of which theme is active.
constexpr int BTN_W = 106;
constexpr int BTN_H = 40;
constexpr int X4_BTN_POS[] = {25, 130, 245, 350};
constexpr int X3_BTN_POS[] = {38, 154, 268, 384};

// Characters we prewarm into every SD-loaded role font on each render.
// Covers printable ASCII, Latin-1 Supplement, and the typography
// punctuation that book metadata commonly uses (smart quotes, em-dash,
// ellipsis, middle dot). Anything outside this set (e.g. CJK in a book
// title) still works — it just faults that glyph on first draw rather
// than being preloaded with the alphabet.
//
// ~230 codepoints, well under SdCardFont::MAX_PAGE_GLYPHS (512). Cost is
// a single batched SD read per font per style.
constexpr const char* COMMON_ALPHABET =
    " !\"#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~"
    "·"  // middle dot — header subtitle separator
    "ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖØÙÚÛÜÝÞß"
    "àáâãäåæçèéêëìíîïðñòóôõöøùúûüýþÿ"
    "–—‘’“”…";  // en/em-dash, smart quotes, ellipsis
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

  // SD role fonts were unloaded on the previous onExit() to free RAM for
  // the reader / wherever-we-came-from. Re-discover them now so the role
  // lookups in prewarmFonts() and FolioTheme::resolveFontRole() see them.
  THEME_FONTS.discover(renderer);

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

  // Prewarm SD-loaded role fonts now so every render afterward is cheap.
  // The mini glyph cache survives across renders within this activity
  // (we don't use FontCacheManager::PrewarmScope and nothing in the
  // Library navigation path clears the cache), so a single prewarm at
  // entry is enough. Coming back from the reader re-enters this activity
  // — onEnter runs again, re-prewarming whatever PrewarmScope wiped.
  prewarmFonts();

  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();

  // The X4 only has ~380 KB of RAM and LibraryActivity holds a lot of it.
  // Whatever comes next (most often the reader, which needs every byte it
  // can get for EPUB indexing) deserves the full heap back. We re-load
  // everything from disk on the next onEnter().
  //
  // Tally of what's released:
  //   ~13 KB  cover bitmap cache (9 framebuffer-region buffers)
  //   ~30 KB  SD-font mini glyph data (5 role fonts × ~240 codepoints prewarmed)
  //   ~50 KB  SD-font persistent state (intervals + kerning tables for
  //           three installed role fonts × 4 styles each)
  //   ~25 KB  LibraryIndex books vector
  //   --------
  //   ~118 KB total
  invalidateCoverCache();

  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();

  THEME_FONTS.unloadAll(renderer);
  LIBRARY_INDEX.unload();
}

void LibraryActivity::invalidateCoverCache() {
  for (auto& c : coverCache) {
    c.buf.reset();
    c.size = 0;
    c.valid = false;
  }
  cachedCoverPage = -1;
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
  if (row < ROWS - 1) {
    const int candidate = (row + 1) * COLS + col;
    // If next row is partial (empty cells), only move if there's a book there.
    if (LIBRARY_INDEX.getAt(libraryPage, candidate, PER_PAGE) != nullptr) {
      librarySelected = candidate;
      requestUpdate();
    }
  } else if (libraryPage < totalPages() - 1) {
    libraryPage++;
    librarySelected = col;
    requestUpdate();
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
  // Fonts were prewarmed in onEnter() — drawText calls below hit the warm
  // mini glyph cache, no per-glyph SD faults. See prewarmFonts() for the
  // cache-lifetime reasoning.
  renderer.clearScreen();
  renderPasses();
  renderer.displayBuffer();
}

void LibraryActivity::prewarmFonts() {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  // Bitmask convention: 0x01=regular, 0x02=bold, 0x04=italic, 0x08=bold-italic.
  constexpr uint8_t REG_BOLD = 0x03;
  constexpr uint8_t ITALIC = 0x04;
  constexpr uint8_t BOLD_ITALIC = 0x06;

  // Per role: which styles do we actually render in? Tighter masks mean
  // fewer glyphs prewarmed. The text content itself is the shared alphabet
  // — anything beyond it (CJK, rare Latin Extended) faults lazily on draw,
  // which is fine because it's rare and one-time.
  fcm->prewarmCache(FolioTheme::resolveFontRole(FontRole::Caption), COMMON_ALPHABET, BOLD_ITALIC);
  fcm->prewarmCache(FolioTheme::resolveFontRole(FontRole::Title), COMMON_ALPHABET, REG_BOLD);
  fcm->prewarmCache(FolioTheme::resolveFontRole(FontRole::Heading), COMMON_ALPHABET, ITALIC);
  fcm->prewarmCache(FolioTheme::resolveFontRole(FontRole::Body), COMMON_ALPHABET, ITALIC);
  fcm->prewarmCache(FolioTheme::resolveFontRole(FontRole::Accent), COMMON_ALPHABET, ITALIC);
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

  // Footer button hints. Labels depend on the active view. Note that the
  // physical Back button is what we label "Menu" in Library / "Back" in Menu —
  // see the loop() handler for the mapping rationale.
  const char* backLabel = (view == View::Menu) ? tr(STR_BACK) : tr(STR_MENU_LABEL);
  const char* leftLabel = (view == View::Menu) ? "" : tr(STR_DIR_LEFT);
  const char* rightLabel = (view == View::Menu) ? "" : tr(STR_DIR_RIGHT);
  const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), leftLabel, rightLabel);

  const int pageHeight = renderer.getScreenHeight();
  const int* positions = gpio.deviceIsX3() ? X3_BTN_POS : X4_BTN_POS;
  const char* btnLabels[] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};
  for (int i = 0; i < 4; ++i) {
    if (btnLabels[i] == nullptr || btnLabels[i][0] == '\0') continue;
    FolioTheme::drawFolioButtonHint(renderer, positions[i], pageHeight - BTN_H, BTN_W, BTN_H, btnLabels[i]);
  }
}

void LibraryActivity::renderHeader() {
  // 3px inner bottom border at the bottom of the 89-px header band.
  renderer.fillRect(0, HEADER_HEIGHT - HEADER_BOTTOM_BORDER, renderer.getScreenWidth(), HEADER_BOTTOM_BORDER);

  // Battery icon top-right. Delegates to whichever theme is active —
  // intentional, the battery is one of the few elements LibraryActivity
  // happily inherits from the user's chosen theme.
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(renderer.getScreenWidth() - maxBatteryWidth, 5, maxBatteryWidth, metrics.batteryHeight + 10, false);
  const bool showBatteryPct =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = renderer.getScreenWidth() - 12 - metrics.batteryWidth;
  GUI.drawBatteryRight(renderer, Rect{batteryX, 5, metrics.batteryWidth, metrics.batteryHeight}, showBatteryPct);

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

  const int titleFont = FolioTheme::resolveFontRole(FontRole::Title);
  const int captionFont = FolioTheme::resolveFontRole(FontRole::Caption);

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

  // If we're rendering a different page than what's cached, drop the per-
  // slot buffers. They'll be rebuilt by the first renderBookTile call below.
  if (cachedCoverPage != libraryPage) {
    invalidateCoverCache();
    cachedCoverPage = libraryPage;
  }

  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, slot, PER_PAGE);
    if (book == nullptr) continue;
    const Rect cell = cellRect(slot / COLS, slot % COLS, shelfX, shelfY, shelfW, shelfH);
    const bool selected = (slot == librarySelected);

    // Tile content first, selection frame on top. The frame is now just an
    // outline + corner brackets (no hatch background since 1-bit can't
    // reproduce 5% opacity legibly), so it works as a foreground overlay.
    // Drawing the frame last guarantees nothing — not the cover-region
    // memcpy from the cache, not the text below — can clip its edges.
    renderBookTile(slot, *book, selected);
    if (selected) {
      const Rect content = tileContentRect(*book, cell);
      FolioTheme::drawSelectionFrame(renderer, Rect{content.x - 2, content.y - 2, content.width + 4, content.height + 4});
    }
  }
}

Rect LibraryActivity::tileContentRect(const LibraryBook& book, const Rect& cell) const {
  // Mirror the layout math in renderBookTile so the frame and the rendered
  // tile agree on where the content ends. wrappedText is a pure measurement
  // call (no draw); the font caches are warm at this point so it's cheap.
  const int captionFont = FolioTheme::resolveFontRole(FontRole::Caption);
  const int captionLineH = renderer.getLineHeight(captionFont);

  const int coverX = cell.x + (cell.width - COVER_W) / 2;
  const int coverY = cell.y + 4;

  const int progressArea = book.hasProgress() ? (TILE_PROGRESS_HEIGHT + 2 + TILE_PROGRESS_MARGIN_TOP) : 0;
  const int textBudget = cell.height - (coverY - cell.y) - COVER_H - 2 /*pad-bottom*/ - progressArea;
  const int titleSpace =
      textBudget - TILE_TITLE_MARGIN_TOP - (book.author.empty() ? 0 : (captionLineH + TILE_AUTHOR_MARGIN_TOP));
  const int maxTitleLines = std::max(1, titleSpace / captionLineH);

  const int titleLineCount = static_cast<int>(std::min<size_t>(
      2, renderer.wrappedText(captionFont, book.title.c_str(), cell.width - 8, std::min(maxTitleLines, 2),
                              EpdFontFamily::BOLD)
             .size()));

  const int titleY = coverY + COVER_H + TILE_TITLE_MARGIN_TOP;
  int bottom = titleY + titleLineCount * captionLineH;
  if (!book.author.empty()) {
    bottom += TILE_AUTHOR_MARGIN_TOP + captionLineH;
  }
  if (book.hasProgress()) {
    bottom += TILE_PROGRESS_MARGIN_TOP + TILE_PROGRESS_HEIGHT + 2;
  }

  // Frame the area that visibly contains content: cell-wide horizontally
  // (text is centered within), starting at the cover's top edge.
  return Rect{cell.x, coverY, cell.width, bottom - coverY};
}

void LibraryActivity::renderBookTile(int slotIndex, const LibraryBook& book, bool /*selected*/) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int shelfX = CONTENT_PAD_X;
  const int shelfY = HEADER_HEIGHT + CONTENT_PAD_Y;
  const int shelfW = screenW - CONTENT_PAD_X * 2 - RAIL_WIDTH - RAIL_GAP;
  const int shelfH = screenH - HEADER_HEIGHT - FOOTER_HEIGHT - CONTENT_PAD_Y * 2;
  const Rect cell = cellRect(slotIndex / COLS, slotIndex % COLS, shelfX, shelfY, shelfW, shelfH);

  // Resolve fonts up front — they may come from SD overrides (smaller faces),
  // in which case the tile content height shrinks and we can fit more lines.
  const int captionFont = FolioTheme::resolveFontRole(FontRole::Caption);
  const int captionLineH = renderer.getLineHeight(captionFont);

  // ---- Cover (centered horizontally within the cell, top-aligned) -----
  // Cover pixels don't depend on selection state (the selection outline is
  // drawn at the cell edges, well outside the cover area). We capture each
  // slot's cover region to a heap buffer on first render of a page and
  // memcpy it back on subsequent renders — the difference between ~300 ms
  // of SD I/O per selection change and zero.
  //
  // Capture rect is intentionally +1 wider and +1 taller than the cover so
  // the bottom and right drop-shadow lines (at coverX+COVER_W, coverY+
  // COVER_H — one pixel outside the cover proper) live inside the cached
  // region. Without that, cache hits restore the cover correctly but lose
  // the shadow lines, which clearScreen has wiped.
  const int coverX = cell.x + (cell.width - COVER_W) / 2;
  const int coverY = cell.y + 4;
  constexpr int CAPTURE_W = COVER_W + 1;
  constexpr int CAPTURE_H = COVER_H + 1;
  CoverCache& slotCache = coverCache[slotIndex];
  const bool cacheHit = (cachedCoverPage == libraryPage && slotCache.valid);

  if (cacheHit) {
    renderer.copyBufferToRegion(coverX, coverY, CAPTURE_W, CAPTURE_H, slotCache.buf.get(), slotCache.size);
  } else {
    // Clear the cover area to white before drawing — drawBitmap only writes
    // black pixels, so without this the previous frame's hatch (or anything
    // else underneath) bleeds through the cover's white regions.
    renderer.fillRect(coverX, coverY, COVER_W, COVER_H, false);

    // Frame rect for the border + drop shadow. Defaults to the full slot
    // (for the title-only fallback) and shrinks to the bitmap's actual
    // drawn dimensions when we have a real cover — so the border hugs the
    // art instead of floating around a centered image with padding.
    int frameX = coverX, frameY = coverY, frameW = COVER_W, frameH = COVER_H;

    bool drewCover = false;
    const std::string thumbPath =
        std::string("/.crosspoint/epub_") + std::to_string(book.pathHash) + "/thumb_144.bmp";
    FsFile file;
    if (Storage.openFileForRead(LOG_TAG, thumbPath.c_str(), file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        // generateThumbBmp targets a 0.6-aspect box (height*0.6 wide, height
        // tall) and fits the source cover into it preserving the source's
        // own aspect — meaning thumbs can be narrower OR shorter than our
        // 2:3 slot depending on the original cover. drawBitmap1Bit only
        // downscales, so we replicate the math here to compute the final
        // size, center the bitmap, and frame the border around it.
        const int bmpW = bitmap.getWidth();
        const int bmpH = bitmap.getHeight();
        float scale = 1.0f;
        if (bmpW > COVER_W) scale = static_cast<float>(COVER_W) / static_cast<float>(bmpW);
        if (bmpH > COVER_H) scale = std::min(scale, static_cast<float>(COVER_H) / static_cast<float>(bmpH));
        const int drawnW = static_cast<int>(bmpW * scale);
        const int drawnH = static_cast<int>(bmpH * scale);
        frameX = coverX + (COVER_W - drawnW) / 2;
        frameY = coverY + (COVER_H - drawnH) / 2;
        frameW = drawnW;
        frameH = drawnH;
        renderer.drawBitmap(bitmap, frameX, frameY, COVER_W, COVER_H);
        drewCover = true;
      }
    }
    if (!drewCover) {
      // Fallback: title-only "cover" — outlined rectangle with the first
      // line of the title centered inside. Uses the full slot.
      const std::string trunc =
          renderer.truncatedText(captionFont, book.title.c_str(), COVER_W - 12, EpdFontFamily::BOLD);
      const int tw = renderer.getTextWidth(captionFont, trunc.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(captionFont, coverX + (COVER_W - tw) / 2, coverY + (COVER_H - captionLineH) / 2,
                        trunc.c_str(), true, EpdFontFamily::BOLD);
    }
    // 1px outer border + 1px offset drop shadow on the cover (Folio motif).
    // Wraps the actual drawn area (bitmap or title fallback), not the slot.
    renderer.drawRect(frameX, frameY, frameW, frameH);
    renderer.drawLine(frameX + 1, frameY + frameH, frameX + frameW, frameY + frameH);
    renderer.drawLine(frameX + frameW, frameY + 1, frameX + frameW, frameY + frameH);

    // Capture the finished cover region (cover + border + drop shadow) so
    // the next render on this page can skip everything above.
    const size_t need = renderer.getRegionByteSize(coverX, coverY, CAPTURE_W, CAPTURE_H);
    if (need > 0 && slotCache.size != need) {
      slotCache.buf = makeUniqueNoThrow<uint8_t[]>(need);
      slotCache.size = slotCache.buf ? need : 0;
    }
    if (slotCache.buf &&
        renderer.copyRegionToBuffer(coverX, coverY, CAPTURE_W, CAPTURE_H, slotCache.buf.get(), slotCache.size)) {
      slotCache.valid = true;
    } else {
      slotCache.valid = false;
    }
  }

  // ---- Adaptive layout for text below the cover -------------------------
  // The cell budget below the cover is the row track minus the cover, the
  // tile padding, and the progress bar's own area. We then divide by the
  // caption font's actual line height to decide how many title lines fit.
  const int progressArea = book.hasProgress() ? (TILE_PROGRESS_HEIGHT + 2 + TILE_PROGRESS_MARGIN_TOP) : 0;
  const int textBudget = cell.height - (coverY - cell.y) - COVER_H - 2 /*pad-bottom*/ - progressArea;
  const int titleSpace = textBudget - TILE_TITLE_MARGIN_TOP - (book.author.empty() ? 0 : (captionLineH + TILE_AUTHOR_MARGIN_TOP));
  const int maxTitleLines = std::max(1, titleSpace / captionLineH);

  // ---- Title (caption font, bold, up to maxTitleLines, centered) -------
  const int titleY = coverY + COVER_H + TILE_TITLE_MARGIN_TOP;
  std::vector<std::string> titleLines = renderer.wrappedText(captionFont, book.title.c_str(), cell.width - 8,
                                                              std::min(maxTitleLines, 2), EpdFontFamily::BOLD);
  for (size_t i = 0; i < titleLines.size(); ++i) {
    const int lineW = renderer.getTextWidth(captionFont, titleLines[i].c_str(), EpdFontFamily::BOLD);
    renderer.drawText(captionFont, cell.x + (cell.width - lineW) / 2,
                      titleY + static_cast<int>(i) * captionLineH, titleLines[i].c_str(), true, EpdFontFamily::BOLD);
  }

  // ---- Author (caption font, italic, 1 line) ----------------------------
  const int authorY =
      titleY + static_cast<int>(titleLines.size()) * captionLineH + TILE_AUTHOR_MARGIN_TOP;
  if (!book.author.empty()) {
    const std::string author =
        renderer.truncatedText(captionFont, book.author.c_str(), cell.width - 8, EpdFontFamily::ITALIC);
    const int aw = renderer.getTextWidth(captionFont, author.c_str(), EpdFontFamily::ITALIC);
    renderer.drawText(captionFont, cell.x + (cell.width - aw) / 2, authorY, author.c_str(), true,
                      EpdFontFamily::ITALIC);
  }

  // ---- Progress bar -----------------------------------------------------
  if (book.hasProgress()) {
    const int barY = authorY + (book.author.empty() ? 0 : captionLineH) + TILE_PROGRESS_MARGIN_TOP;
    const int barX = cell.x + (cell.width - COVER_W) / 2;
    renderer.drawRect(barX, barY, COVER_W, TILE_PROGRESS_HEIGHT + 2);
    const int fillW = (COVER_W - 4) * book.progressPercent() / 100;
    if (fillW > 0) {
      renderer.fillRect(barX + 2, barY + 2, fillW, TILE_PROGRESS_HEIGHT - 2);
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
  // editorial vertical-marginalia feel from the prototype.
  const int accentFont = FolioTheme::resolveFontRole(FontRole::Accent);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", libraryPage + 1, pages);
  const int countY = railBottom - 4;
  const int countX = railX + RAIL_WIDTH / 2 + 4;
  renderer.drawTextRotated90CW(accentFont, countX, countY, countBuf, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderEmptyState() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int font = FolioTheme::resolveFontRole(FontRole::Heading);
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

  const int titleFont = FolioTheme::resolveFontRole(FontRole::Title);
  const int headingFont = FolioTheme::resolveFontRole(FontRole::Heading);
  const int captionFont = FolioTheme::resolveFontRole(FontRole::Caption);

  for (int i = 0; i < MENU_ITEMS; ++i) {
    const int y = areaY + i * (itemH + gap);
    const Rect item{itemX, y, itemW, itemH};
    const bool selected = (i == menuSelected);
    if (selected) {
      FolioTheme::drawSelectionFrame(renderer, item);
    }

    // Numeral (heading-sized italic, dimmer feel than the label).
    const int numW = renderer.getTextWidth(headingFont, romanLabels[i], EpdFontFamily::ITALIC);
    renderer.drawText(headingFont, item.x + 16, item.y + 16, romanLabels[i], true, EpdFontFamily::ITALIC);

    // Label — title-sized bold serif (matches the prototype's 28-px primary).
    const int labelX = item.x + 16 + numW + 14;
    renderer.drawText(titleFont, labelX, item.y + 14, labels[i], true, EpdFontFamily::BOLD);

    // Hairline rule beneath the label (cover-type motif).
    const int ruleY = item.y + 14 + renderer.getLineHeight(titleFont) + 4;
    renderer.drawLine(labelX, ruleY, labelX + 30, ruleY);

    // Meta line (caption italic, secondary).
    renderer.drawText(captionFont, labelX, ruleY + 6, metas[i], true, EpdFontFamily::ITALIC);
  }
}
