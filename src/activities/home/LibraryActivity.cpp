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
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.generated.h"
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

// Resolve font roles against the currently-active theme so Library follows
// whichever theme the user has selected. Each theme provides its own role
// mapping (and SD-installed face, if any) via ThemeData; Library now picks
// up sans-serif Lyra fonts under Lyra, Folio serif under Folio, etc.
int libFont(FontRole role) { return GUI.getFontForRole(role); }

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

// constexpr int RAIL_POWER_HINT_BOTTOM = 225;
constexpr int RAIL_TOP_GAP = 10;

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
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  initPopup();

  // The on-disk library.bin survives onExit; load it back into memory.
  LIBRARY_INDEX.loadFromFile();

  // Always do a refresh pass — picks up newly added EPUBs, drops removed ones,
  // and refreshes progress. New EPUBs trigger a blocking "Indexing library..."
  // popup; the popup never appears when nothing has changed.
  LIBRARY_INDEX.refreshFromSdCard(&renderer);

  // Apply persisted sort. LibraryIndex no longer pre-sorts in
  // refreshFromSdCard — order is the LibraryActivity's responsibility now.
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                      static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));

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

void LibraryActivity::initPopup() {
  // Configure the cascading popup. Top rows: Sort (→ submenu), Files (→
  // submenu), Settings (leaf). The cascade derives chevron glyphs and the
  // muted-owning-row indicator from this config — LibraryActivity only owns
  // the labels and the action dispatch.
  std::vector<CascadingPopupMenu::SubmenuConfig> subs;
  subs.resize(POPUP_TOP_COUNT);

  // Sort submenu: 4 rows; pre-select the active sort field on entry; show a
  // direction arrow on the active field row.
  subs[POPUP_TOP_SORT].itemCount = POPUP_SORT_COUNT;
  subs[POPUP_TOP_SORT].rowLabel = [](int i) -> const char* {
    switch (i) {
      case 0: return tr(STR_SORT_RECENT);
      case 1: return tr(STR_SORT_TITLE);
      case 2: return tr(STR_SORT_AUTHOR);
      case 3: return tr(STR_SORT_PROGRESS);
    }
    return "";
  };
  subs[POPUP_TOP_SORT].rowGlyph = [](int i) -> PopupMenu::Glyph {
    if (i != SETTINGS.librarySortField) return PopupMenu::Glyph::None;
    return (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
               ? PopupMenu::Glyph::ArrowUp
               : PopupMenu::Glyph::ArrowDown;
  };
  subs[POPUP_TOP_SORT].initialSelection = []() { return static_cast<int>(SETTINGS.librarySortField); };

  // Files submenu: 2 rows; always opens at row 0.
  subs[POPUP_TOP_FILES].itemCount = POPUP_FILES_COUNT;
  subs[POPUP_TOP_FILES].rowLabel = [](int i) -> const char* {
    switch (i) {
      case 0: return tr(STR_BROWSE);
      case 1: return tr(STR_TRANSFER);
    }
    return "";
  };

  // Settings is a leaf — no submenu config needed (itemCount=0 default).
  popup_.configure(
      [](int i) -> const char* {
        switch (i) {
          case POPUP_TOP_SORT: return tr(STR_SORT);
          case POPUP_TOP_FILES: return tr(STR_FILES);
          case POPUP_TOP_SETTINGS: return tr(STR_SETTINGS_TITLE);
        }
        return "";
      },
      std::move(subs));
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

  if (lockNextBackRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      lockNextBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if(popup_.isOpen()) {
      popup_.moveLeft();
    } else {
      popup_.open();
    }

    requestUpdate();
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
  if (popup_.isOpen()) {
    if (popup_.moveUp() != CascadingPopupMenu::Nav::Ignored) requestUpdate();
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
  if (popup_.isOpen()) {
    if (popup_.moveDown() != CascadingPopupMenu::Nav::Ignored) requestUpdate();
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
  if (popup_.isOpen()) {
    return;
  }

  const int col = currentCol();
  if(col > 0) {
    librarySelected--;
    requestUpdate();
    return;
  }


  for(int candidate = librarySelected + COLS - 1; candidate >= librarySelected; candidate--) {
    if (LIBRARY_INDEX.getAt(libraryPage, candidate, PER_PAGE) == nullptr) {
      continue;
    }

    librarySelected = candidate;
    requestUpdate();
    return;
  }
}

void LibraryActivity::moveRight() {
  if (popup_.isOpen()) {
    return;
  }

  const int col = currentCol();
  if (col == COLS - 1) {
    librarySelected -= (COLS - 1);
    requestUpdate();
    return;
  }

  const int candidate = librarySelected + 1;
  if (LIBRARY_INDEX.getAt(libraryPage, candidate, PER_PAGE) != nullptr) {
    librarySelected = candidate;
    requestUpdate();
  }
}

void LibraryActivity::doSelect() {
  if (popup_.isOpen()) {
    const auto nav = popup_.activate();
    if (nav == CascadingPopupMenu::Nav::EnteredSubmenu) {
      requestUpdate();
    } else if (nav == CascadingPopupMenu::Nav::LeafActivated ||
               nav == CascadingPopupMenu::Nav::SubItemActivated) {
      dispatchPopupActivation(nav);
    }
    return;
  }

  const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, librarySelected, PER_PAGE);
  if (book == nullptr) return;
  LOG_DBG(LOG_TAG, "Opening book: %s", book->path.c_str());

  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();

  activityManager.goToReader(book->path);
}

void LibraryActivity::dispatchPopupActivation(CascadingPopupMenu::Nav navResult) {
  const int top = popup_.topSelectedIndex();
  if (navResult == CascadingPopupMenu::Nav::LeafActivated) {
    // Only Settings is a leaf at the top level.
    if (top == POPUP_TOP_SETTINGS) {
      activityManager.goToSettings();
    }
    return;
  }
  // SubItemActivated: dispatch by which submenu the user is in.
  const int sub = popup_.subSelectedIndex();
  if (top == POPUP_TOP_SORT) {
    // If the user re-confirms the active sort field, flip direction;
    // otherwise switch the active field and keep the persisted direction.
    const uint8_t newField = static_cast<uint8_t>(sub);
    uint8_t newDirection = SETTINGS.librarySortDirection;
    if (newField == SETTINGS.librarySortField) {
      newDirection = (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
                         ? CrossPointSettings::LIB_SORT_DESC
                         : CrossPointSettings::LIB_SORT_ASC;
    }
    setSort(newField, newDirection);
  } else if (top == POPUP_TOP_FILES) {
    if (sub == POPUP_FILES_BROWSE) {
      activityManager.goToFileBrowser();
    } else if (sub == POPUP_FILES_TRANSFER) {
      activityManager.goToFileTransfer();
    }
  }
}

void LibraryActivity::applySort() {
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                      static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));
  libraryPage = 0;
  librarySelected = 0;
  requestUpdate();
}

void LibraryActivity::setSort(uint8_t field, uint8_t direction) {
  const bool changed =
      (field != SETTINGS.librarySortField) || (direction != SETTINGS.librarySortDirection);
  if (changed) {
    SETTINGS.librarySortField = field;
    SETTINGS.librarySortDirection = direction;
    SETTINGS.saveToFile();
  }
  applySort();
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

  (void)headingFont;  // (kept for future popup chrome that may need numerals)

  // The library shelf always paints underneath — the popup floats above it,
  // so the shelf glyphs need to be declared regardless of view.
  tc.use(titleFont, EpdFontFamily::BOLD, tr(STR_LIBRARY));
  if (!LIBRARY_INDEX.isEmpty()) {
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_SORTED_RECENT));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_SORTED_TITLE));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_SORTED_AUTHOR));
    tc.use(captionFont, EpdFontFamily::ITALIC, tr(STR_LIBRARY_SORTED_PROGRESS));
    tc.use(captionFont, EpdFontFamily::ITALIC, "0123456789 ()ascde · /");
    // Page rail "1 / 12" — uses the compact accent face.
    tc.use(accentCompactFont, EpdFontFamily::ITALIC, "0123456789 /");
  }
  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const LibraryBook* book = LIBRARY_INDEX.getAt(libraryPage, slot, PER_PAGE);
    if (book == nullptr) continue;
    tc.use(captionCompactFont, EpdFontFamily::BOLD, book->title);
    if (!book->author.empty()) {
      tc.use(captionCompactFont, EpdFontFamily::ITALIC, book->author);
    }
  }

  // Popup row labels are owned by the cascade — it walks the configured
  // top + submenu rows itself. No-op when closed.
  popup_.declareText(tc);

  // Button hints — italic body labels, compact face so the label has
  // breathing room inside the 106-px Folio hint box.
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_MENU_LABEL));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_BACK));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_SELECT));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_ENTER));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_CLOSE));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_DIR_LEFT));
  tc.use(bodyCompactFont, EpdFontFamily::ITALIC, tr(STR_DIR_RIGHT));
}

void LibraryActivity::renderPasses() {
  renderHeader();
  if (LIBRARY_INDEX.isEmpty()) {
    renderEmptyState();
  } else {
    renderLibraryShelf();
    renderPageRail();
  }

  if (popup_.isOpen()) {
    renderPopup();
  }

  // Side-rail power-button hint. Only the power slot is populated — the
  // page-turn side buttons aren't bound in Library, so their hint slots
  // stay empty.
  // ButtonHints::renderSide(renderer, "", "", tr(STR_DIR_RIGHT));

  // Library-view footer hints. When the popup is open the cascade owns the
  // hint scheme (Close/Back, Select/Enter — see CascadingPopupMenu::renderFooterHints).
  if (popup_.isOpen()) {
    popup_.renderFooterHints(renderer, mappedInput);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_MENU_LABEL), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                              tr(STR_DIR_RIGHT));
    ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

bool LibraryActivity::handlePowerShortPress() {
  // Short-press of the power button is bound to "move right" — mirrors the
  // Right front-button binding (popup navigation when open, grid otherwise).
  // Consuming the press suppresses the global FORCE_REFRESH dispatch.
  moveRight();
  return true;
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
  const char* title = tr(STR_LIBRARY);

  std::string subtitleText;
  if (!LIBRARY_INDEX.isEmpty()) {
    StrId sortedKey = StrId::STR_LIBRARY_SORTED_RECENT;
    switch (SETTINGS.librarySortField) {
      case CrossPointSettings::LIB_SORT_TITLE:    sortedKey = StrId::STR_LIBRARY_SORTED_TITLE; break;
      case CrossPointSettings::LIB_SORT_AUTHOR:   sortedKey = StrId::STR_LIBRARY_SORTED_AUTHOR; break;
      case CrossPointSettings::LIB_SORT_PROGRESS: sortedKey = StrId::STR_LIBRARY_SORTED_PROGRESS; break;
      case CrossPointSettings::LIB_SORT_RECENT:
      default:                                    sortedKey = StrId::STR_LIBRARY_SORTED_RECENT; break;
    }
    // Direction marker uses ASCII so it renders in any font. The popup-menu
    // primitive paints proper triangle glyphs for the in-popup arrows; the
    // subtitle stays light-touch text.
    const char* arrow =
        (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC) ? "(asc)" : "(desc)";
    subtitleText = std::string(I18n::getInstance().get(sortedKey)) + " " + arrow + "  ·  " +
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

  const auto& lib = GUI.getData()->library;

  // Default frame for the title-only fallback (no thumb on disk).
  int frameX = cell.x + (cell.width - COVER_W) / 2;
  int frameY = coverY;
  int frameW = COVER_W;
  int frameH = COVER_H;

  char thumbPath[64];
  snprintf(thumbPath, sizeof(thumbPath), "/.crosspoint/epub_%lu/thumb_144.bmp",
           static_cast<unsigned long>(book.pathHash));

  GfxRenderer::CachedBitmap* thumbHandle = renderer.lookupCachedBitmap(thumbPath);

  int bmpW = 0, bmpH = 0;
  const bool haveThumb = renderer.getCachedBitmapDimensions(thumbHandle, &bmpW, &bmpH) && bmpW > 0 && bmpH > 0;

  if (haveThumb) {
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
  }

  // Drop shadow first — the white substrate fill below overpaints the part
  // underneath the cover, leaving only the offset L-shape visible. Drawing
  // a full rounded rect (vs. two offset lines) means the shadow's curve
  // naturally matches the cover's when coverBorderRadius > 0.
  if (lib.coverDropShadowOffsetX > 0 || lib.coverDropShadowOffsetY > 0) {
    renderer.fillRoundedRect(frameX + lib.coverDropShadowOffsetX, frameY + lib.coverDropShadowOffsetY,
                             frameW, frameH, lib.coverBorderRadius,
                             invertText ? Color::White : Color::Black);
  }

  // White substrate. drawCachedBitmap only writes black pixels, so the white
  // fill keeps the selection background showing in the slot margins around
  // the cover — and overpaints the shadow rect everywhere except the
  // offset L-shape poking out the bottom-right.
  renderer.fillRect(frameX, frameY, frameW, frameH, false);

  bool drewCover = false;
  if (haveThumb) {
    drewCover = renderer.drawCachedBitmap(thumbHandle, frameX, frameY, frameW, frameH);
  }
  if (!drewCover) {
    // Fallback: title-only "cover" — first line of the title centered inside
    // the (already-filled) frame.
    const std::string trunc =
        renderer.truncatedText(captionFont, book.title.c_str(), COVER_W - 12, EpdFontFamily::BOLD);
    const int tw = renderer.getTextWidth(captionFont, trunc.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(captionFont, frameX + (COVER_W - tw) / 2, frameY + (COVER_H - captionLineH) / 2,
                      trunc.c_str(), true, EpdFontFamily::BOLD);
  }

  // Clip the (square) bitmap to the rounded shape — drawCachedBitmap paints
  // black into the corner pixels outside the curve, which would poke past
  // the rounded border. Mask color is the backdrop: black when the selection
  // inverts text (dark fill behind the tile), white otherwise.
  if (lib.coverBorderRadius > 0) {
    renderer.maskRoundedRectOutsideCorners(frameX, frameY, frameW, frameH, lib.coverBorderRadius,
                                           invertText ? Color::Black : Color::White);
  }

  // Border last, on top of the bitmap. drawRoundedRect with radius 0
  // degenerates to a plain rectangle, preserving Folio's square look.
  if (lib.coverBorderWidth > 0) {
    renderer.drawRoundedRect(frameX, frameY, frameW, frameH, lib.coverBorderWidth, lib.coverBorderRadius,
                             !invertText);
  }

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
  
  // Anchor the rail below the power-button hint slot — see RAIL_POWER_HINT_BOTTOM.
  // const int railTop = RAIL_POWER_HINT_BOTTOM + RAIL_TOP_GAP;
  const int railTop = HEADER_HEIGHT + CONTENT_PAD_Y + RAIL_PAD_TOP;
  const int railBottom = screenH - FOOTER_HEIGHT - CONTENT_PAD_Y;

  const auto& lib = GUI.getData()->library;
  const int tickSize = lib.pageIndicatorSize;
  const int visibleTicks = std::min(pages, RAIL_TICK_COUNT);
  const int tickX = railX + (RAIL_WIDTH - tickSize) / 2;
  for (int i = 0; i < visibleTicks; ++i) {
    const int tickY = railTop + i * (tickSize + lib.pageIndicatorGap);

    const Color fill = (i == libraryPage) ? lib.pageIndicatorFillSelected : lib.pageIndicatorFill;
    const Color border = (i == libraryPage) ? lib.pageIndicatorBorderSelected : lib.pageIndicatorBorder;

    switch(lib.pageIndicatorShape) {
      case IndicatorShape::Circle: {
        const int r = tickSize / 2;
        const int cx = tickX + r;
        const int cy = tickY + r;
        renderer.fillCircle(cx, cy, r, fill);
        renderer.drawCircle(cx, cy, r, border);
        break;
      } 
      case IndicatorShape::Square:
        renderer.fillRectDither(tickX, tickY, tickSize, tickSize, fill);
        renderer.drawRect(tickX, tickY, tickSize, tickSize, border == Color::Black);
        break;
      case IndicatorShape::RoundedRect:
        renderer.fillRoundedRect(tickX, tickY, tickSize, tickSize, lib.pageIndicatorCornerRadius, fill);
        renderer.drawRoundedRect(tickX, tickY, tickSize, tickSize, 1, lib.pageIndicatorCornerRadius,
                                 border == Color::Black);
        break;
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

void LibraryActivity::renderPopup() {
  // Cascading popup over the library shelf, anchored above the footer hints.
  // The cascade computes its own panel widths and heights from the active
  // theme's metrics and the registered submenus — LibraryActivity just
  // supplies the anchor point and the available right edge for the sub-panel.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int leftX = CONTENT_PAD_X + 12;
  const int bottomLimit = screenH - FOOTER_HEIGHT - 6;
  const int rightLimit = screenW - CONTENT_PAD_X;
  popup_.render(renderer, leftX, bottomLimit, rightLimit);
}
