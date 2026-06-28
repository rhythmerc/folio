#include "LibraryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "LibrarySubsetManager.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/RenderLock.h"
#include "activities/home/CollectionPickerActivity.h"
#include "activities/home/CollectionsActivity.h"
#include "activities/home/LibraryGridView.h"
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
#include "components/ui/UIPage/UIPage.h"
#include "stores/collections/CollectionStore.h"
#include "components/ui/TextBlock/TextBlock.h"
#include "util/Flex.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

int libFont(FontRole role) { return GUI.getFontForRole(role); }

// Page scaffold geometry: body inner padding, the page-indicator rail, and its top inset.
// The grid/tile layout constants live in LibraryGridView.
constexpr int CONTENT_PAD_X = 18;
constexpr int CONTENT_PAD_Y = 8;
constexpr int RAIL_WIDTH = 18;
constexpr int RAIL_GAP = 10;
constexpr int RAIL_PAD_TOP = 8;
static constexpr StrId kSortLabels[] = {
  StrId::STR_SORT_RECENT,
  StrId::STR_SORT_TITLE,
  StrId::STR_SORT_AUTHOR,
  StrId::STR_SORT_PROGRESS
};

// Translate the persisted view settings into a subset spec (deserialization, not
// filtering). The string_views point into SETTINGS.libraryViewName, which outlives the
// resolve() call they're handed to.
LibrarySubsetManager::Spec specFromSettings() {
  using SM = LibrarySubsetManager;
  switch (SETTINGS.libraryViewKind) {
    case CrossPointSettings::LIB_VIEW_COLLECTION:
      return SM::CollectionRef{SETTINGS.libraryViewCollectionId};
    case CrossPointSettings::LIB_VIEW_SERIES:
      return SM::MetadataGroup{SM::MetadataGroup::Series, SETTINGS.libraryViewName};
    case CrossPointSettings::LIB_VIEW_AUTHOR:
      return SM::MetadataGroup{SM::MetadataGroup::Author, SETTINGS.libraryViewName};
    case CrossPointSettings::LIB_VIEW_GENRE:
      return SM::MetadataGroup{SM::MetadataGroup::Genre, SETTINGS.libraryViewName};
    case CrossPointSettings::LIB_VIEW_SEARCH:
      return SM::Search{SETTINGS.libraryViewName};
    default:
      return SM::All{};
  }
}

bool isAutoGroup(uint8_t kind) {
  return kind == CrossPointSettings::LIB_VIEW_SERIES ||
         kind == CrossPointSettings::LIB_VIEW_AUTHOR ||
         kind == CrossPointSettings::LIB_VIEW_GENRE;
}

// Reset the persisted active view to All Books and save. Used as the fallback when the
// active view no longer resolves to anything (stale collection / emptied auto-group).
void resetViewToAll() {
  SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_ALL;
  SETTINGS.libraryViewCollectionId = 0;
  SETTINGS.libraryViewName[0] = '\0';
  SETTINGS.saveToFile();
}
}  // namespace

// ---- Lifecycle --------------------------------------------------------------

void LibraryActivity::onEnter() {
  Activity::onEnter();

  // LIBRARY_INDEX is already populated by LibraryIndexingActivity, which runs first and
  // hands off to us (see ActivityManager::goHome). We're never entered cold.

  // Manual-collection membership store, for the COLLECTION view filter.
  COLLECTION_STORE.loadFromFile();

  // Apply persisted sort, then resolve the active subset and hand it to the grid view.
  LIBRARY_INDEX.sortBy(
    static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
    static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection)
  );

  gridView_.onEnter(computeContentRect(), resolveSubset());
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();

  gridView_.onExit();
  LIBRARY_INDEX.unload();
}

// ---- Collections ------------------------------------------------------------

bool LibraryActivity::openCollectionPicker(bool add) {
  const int idx = gridView_.selectedIndex();
  if (idx < 0) return true;  // back tile / empty slot — nothing to edit

  // Capture by value: the subset may be rebuilt while the picker is up.
  const uint32_t bookHash = LIBRARY_INDEX.getAt(idx).pathHash;
  const auto mode =
    add ? CollectionPickerActivity::Mode::Add : CollectionPickerActivity::Mode::Remove;
  startActivityForResult(
    std::make_unique<CollectionPickerActivity>(renderer, mappedInput, bookHash, mode),
    [this](const ActivityResult&) { onCollectionMembershipChanged(); }
  );
  return true;  // picker launched (pushed) — close the global menu behind it
}

// ---- Input ------------------------------------------------------------------

void LibraryActivity::loop() {
  // The grid view's prefetch worker may have finished a page's covers — repaint to swap
  // placeholders for real artwork.
  if (gridView_.pollCoverUpdates()) requestUpdate();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSelect();
    return;
  }

  // Up/Down (tap + hold) and Left/Right navigation live in the view; it reports whether
  // anything changed so we repaint.
  if (gridView_.handleInput()) requestUpdate();
}

// ---- Selection --------------------------------------------------------------

void LibraryActivity::doSelect() {
  if (gridView_.isBackTileSelected()) {
    activateBack();
    return;
  }

  const int idx = gridView_.selectedIndex();
  if (idx < 0) return;

  const std::string path = LIBRARY_INDEX.getPath(idx);
  if (path.empty()) return;
  LOG_DBG(LOG_TAG, "Opening book: %s", path.c_str());
  activityManager.goToReader(path);
}

bool LibraryActivity::onSearch() {
  const std::string current =
    (SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_SEARCH)
      ? std::string(SETTINGS.libraryViewName)
      : std::string();
  startActivityForResult(
    std::make_unique<KeyboardEntryActivity>(
      renderer,
      mappedInput,
      tr(STR_SEARCH),
      current,
      sizeof(SETTINGS.libraryViewName) - 1,
      InputType::Text
    ),
    [this](const ActivityResult& res) {
      if (!res.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(res.data);
        if (!kb.text.empty()) {
          SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_SEARCH;
          SETTINGS.libraryViewCollectionId = 0;
          strncpy(
            SETTINGS.libraryViewName,
            kb.text.c_str(),
            sizeof(SETTINGS.libraryViewName) - 1
          );
          SETTINGS.libraryViewName[sizeof(SETTINGS.libraryViewName) - 1] = '\0';
          SETTINGS.saveToFile();
          reloadActiveView();
        }
      }
      requestUpdate();
    }
  );
  return true;
}

void LibraryActivity::reloadActiveView() {
  // Re-resolve under the view's cache lock and land on the first real book. The caller
  // repaints (placeholders show until covers load).
  gridView_.setSubset(
    [this] { return resolveSubset(); }, LibraryGridView::InitialSelection::FirstBook
  );
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
  if (viewChanged) reloadActiveView();
  requestUpdate();
}

void LibraryActivity::applySort() {
  // sortBy reorders the index in place (invalidating the subset's pointers), so it must
  // run inside the view's cache lock alongside the re-resolve — setSubset's produce
  // closure does both. Reset selection to the top.
  gridView_.setSubset(
    [this] {
      LIBRARY_INDEX.sortBy(
        static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
        static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection)
      );
      return resolveSubset();
    },
    LibraryGridView::InitialSelection::Top
  );
  requestUpdate();  // placeholders now; real covers on batch-done
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

// ---- Subset resolution ------------------------------------------------------

LibraryGridView::Subset LibraryActivity::resolveSubset() {
  // Degenerate persisted state: an empty search query is just All Books.
  if (
    SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_SEARCH &&
    SETTINGS.libraryViewName[0] == '\0'
  ) {
    resetViewToAll();
  }

  const uint8_t kind = SETTINGS.libraryViewKind;
  std::optional<LibrarySubsetManager::Resolved> resolved =
    LibrarySubsetManager::resolve(specFromSettings());

  // Fallback policy (view/persistence concern, kept in the shell): a stale collection
  // (resolve -> nullopt) or an auto-group that now matches nothing resets to All Books.
  // An empty-but-valid collection and a zero-hit search are valid states — keep them.
  if (!resolved || (isAutoGroup(kind) && resolved->books.empty())) {
    resetViewToAll();
    resolved = LibrarySubsetManager::resolve(LibrarySubsetManager::All{});
  }

  LibraryGridView::Subset subset;
  subset.books = std::move(resolved->books);
  subset.title =
    resolved->title.empty() ? std::string(tr(STR_LIBRARY)) : std::move(resolved->title);
  subset.hasBackTile = (SETTINGS.libraryViewKind != CrossPointSettings::LIB_VIEW_ALL);
  return subset;
}

void LibraryActivity::activateBack() {
  resetViewToAll();
  gridView_.setSubset(
    [this] { return resolveSubset(); }, LibraryGridView::InitialSelection::FirstBook
  );
  requestUpdate();  // placeholders now; real covers on batch-done
}

// ---- Render -----------------------------------------------------------------

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderPasses();
  renderer.displayBuffer();
}

Rect LibraryActivity::computeContentRect() {
  // Replay the page scaffold (UIPage body → body/rail Hstack) without drawing, to get
  // the shelf content rect the grid view lays its tiles out within. Mirrors the render
  // path below (and UIPage::render); keep in sync if either changes.
  const auto& td = *GUI.getData();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};

  flex::Vstack page(
    screen,
    {flex::fixed(td.layout.topPadding),
     flex::fixed(td.header.height),
     flex::grow(),
     flex::fixed(td.buttonHints.height)}
  );
  const Rect body =
    flex::inset(page[2], flex::xy(td.library.pagePaddingX, td.library.pagePaddingY));

  flex::Hstack bodyInner(
    body,
    {flex::grow(), flex::fixed(RAIL_WIDTH)},
    RAIL_GAP,
    flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y)
  );
  return bodyInner[0];
}

void LibraryActivity::renderPasses() {
  const auto& td = *GUI.getData();

  const auto btnLabels = mappedInput.mapLabels(
    tr(STR_MENU_LABEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT)
  );

  const auto body = UIPage::render(
    renderer,
    gridView_.title().c_str(),
    getHeaderSubtitleText().c_str(),
    btnLabels,
    flex::xy(td.library.pagePaddingX, td.library.pagePaddingY)
  );

  // Zero grid items means an empty device library (All Books with no books). A
  // filtered-but-empty view still has its back tile, so it renders the grid body (just
  // that tile).
  if (gridView_.isEmpty()) {
    renderEmptyState(body);
  } else {
    flex::Hstack bodyInner(
      body,
      {flex::grow(), flex::fixed(RAIL_WIDTH)},
      RAIL_GAP,
      flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y)
    );
    gridView_.renderBody(bodyInner[0]);
    renderScrollIndicator(bodyInner[1], gridView_.currentPage(), gridView_.pageCount());
  }
}

bool LibraryActivity::handlePowerShortPress() {
  switch (SETTINGS.libraryPowerButton) {
    case CrossPointSettings::LIB_PWR_NEXT_BOOK:
      gridView_.moveNext();
      break;
    case CrossPointSettings::LIB_PWR_NEXT_IN_ROW:
      gridView_.moveRight();
      break;
  }

  requestUpdate();
  return true;
}

void LibraryActivity::renderBattery(const Rect& headerBox) {
  const auto& td = *GUI.getData();
  constexpr int kBatteryEraseWidth = 80;
  constexpr int kBatteryTopOffset = 5;
  constexpr int kBatteryRightInset = 12;

  const Rect batteryRow{
    headerBox.x,
    headerBox.y + kBatteryTopOffset,
    headerBox.width - kBatteryRightInset,
    td.battery.height
  };

  const Rect batteryRect = flex::align(
    batteryRow,
    td.battery.width,
    td.battery.height,
    flex::HAlign::End,
    flex::VAlign::Start
  );

  const bool showBatteryPct = SETTINGS.hideBatteryPercentage !=
                              CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;

  GUI.drawBatteryRight(renderer, batteryRect, showBatteryPct);
}

std::string LibraryActivity::getHeaderSubtitleText() {
  if (gridView_.isEmpty()) {
    return std::string("");
  }

  const auto& td = *GUI.getData();
  std::string booksText = std::to_string(gridView_.bookCount()) + " " + tr(STR_BOOKS);

  // only left-aligned-bordered leaves enough room for the full text
  // keep it to the book count otherwise
  switch (td.header.style) {
    case HeaderStyle::LeftAlignedBordered:
      break;
    default:
      return booksText;
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

  const char* arrow = (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
                        ? "(asc)"
                        : "(desc)";
  return std::string(I18n::getInstance().get(sortedKey)) + " " + arrow + "  ·  " +
         booksText;
}

void LibraryActivity::renderScrollIndicator(
  const Rect& railArea, uint8_t currentPage, uint8_t pageCount
) {
  const int pages = std::max(1, static_cast<int>(pageCount));
  const int railTop = railArea.y + RAIL_PAD_TOP;
  const int railBottom = railArea.y + railArea.height;

  const auto& lib = GUI.getData()->library;
  const int tickSize = lib.pageIndicatorSize;

  for (int i = 0; i < pages; ++i) {
    const int tickRowY = railTop + i * (tickSize + lib.pageIndicatorGap);
    // Per-tick row slot spans the full rail width; the tick itself is
    // centered horizontally inside it via flex::align.
    const Rect tickRow{railArea.x, tickRowY, railArea.width, tickSize};
    const Rect tick =
      flex::align(tickRow, tickSize, tickSize, flex::HAlign::Center, flex::VAlign::Start);

    const Color fill =
      (i == currentPage) ? lib.pageIndicatorFillSelected : lib.pageIndicatorFill;
    const Color border =
      (i == currentPage) ? lib.pageIndicatorBorderSelected : lib.pageIndicatorBorder;

    switch (lib.pageIndicatorShape) {
      case IndicatorShape::Circle: {
        const int r = tickSize / 2;
        renderer.fillCircle(tick.x + r, tick.y + r, r, fill);
        renderer.drawCircle(tick.x + r, tick.y + r, r, border);
        break;
      }
      case IndicatorShape::Square:
        renderer.fillRectDither(tick.x, tick.y, tick.width, tick.height, fill);
        renderer.drawRect(
          tick.x, tick.y, tick.width, tick.height, border == Color::Black
        );
        break;
      case IndicatorShape::RoundedRect:
        renderer.fillRoundedRect(
          tick.x, tick.y, tick.width, tick.height, lib.pageIndicatorCornerRadius, fill
        );
        renderer.drawRoundedRect(
          tick.x,
          tick.y,
          tick.width,
          tick.height,
          1,
          lib.pageIndicatorCornerRadius,
          border == Color::Black
        );
        break;
    }
  }

  // Page count at the bottom of the rail
  const int accentFont = libFont(FontRole::AccentCompact);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", currentPage + 1, pages);
  const int countY = railBottom - 4;
  const int countX = railArea.x + railArea.width / 2 + 4;
  renderer.drawTextRotated90CW(
    accentFont, countX, countY, countBuf, true, EpdFontFamily::ITALIC
  );
}

void LibraryActivity::renderEmptyState(const Rect& body) {
  const TextBlock::Line lines[]{
    TextBlock::Line{ 
      .text = tr(STR_LIBRARY_NO_BOOKS_TITLE),
      .fontId = libFont(FontRole::Title)
    },
    TextBlock::Line{
      .text = tr(STR_LIBRARY_NO_BOOKS_SUBTITLE),
      .fontId = libFont(FontRole::BodyCompact),
      .gapBefore = 8 
    },
  };

  TextBlock::render(renderer, body, lines, 2);
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

std::optional<PopupMenu::Glyph> LibraryActivity::getSortGlyph(
  CrossPointSettings::LIBRARY_SORT_FIELD sortType
) {
  if (sortType == SETTINGS.librarySortField) {
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
      .icon = {40, 40, Sort40Icon},
      .name = tr(STR_SORT),
      .popupItems =
        {
          PopupMenuEntry{
            .label = tr(STR_SORT_RECENT),
            .glyph = getSortGlyph(SortField::LIB_SORT_RECENT),
            .onSelected =
              [this]() { return this->onSortSelect(SortField::LIB_SORT_RECENT); }
          },
          PopupMenuEntry{
            .label = tr(STR_SORT_TITLE),
            .glyph = getSortGlyph(SortField::LIB_SORT_TITLE),
            .onSelected =
              [this]() { return this->onSortSelect(SortField::LIB_SORT_TITLE); }
          },
          PopupMenuEntry{
            .label = tr(STR_SORT_AUTHOR),
            .glyph = getSortGlyph(SortField::LIB_SORT_AUTHOR),
            .onSelected =
              [this]() { return this->onSortSelect(SortField::LIB_SORT_AUTHOR); }
          },
          PopupMenuEntry{
            .label = tr(STR_SORT_PROGRESS),
            .glyph = getSortGlyph(SortField::LIB_SORT_PROGRESS),
            .onSelected =
              [this]() { return this->onSortSelect(SortField::LIB_SORT_PROGRESS); }
          },
        }
    },
    MenuRegistryEntry{
      .icon = {40, 40, Books40Icon},
      .name = tr(STR_COLLECTIONS),
      .onPress =
        [this]() {
          const uint8_t prevKind = SETTINGS.libraryViewKind;
          const uint32_t prevId = SETTINGS.libraryViewCollectionId;
          const std::string prevName = SETTINGS.libraryViewName;
          startActivityForResult(
            std::make_unique<CollectionsActivity>(renderer, mappedInput),
            [this, prevKind, prevId, prevName](const ActivityResult&) {
              const bool changed = SETTINGS.libraryViewKind != prevKind ||
                                   SETTINGS.libraryViewCollectionId != prevId ||
                                   prevName != std::string(SETTINGS.libraryViewName);
              onReturnFromCollections(changed);
            }
          );
          return true;
        }
    },
    MenuRegistryEntry{
      .icon = {40, 40, Search40Icon}, .name = tr(STR_SEARCH), .onPress = [this]() {
        return onSearch();
      }
    }
  };

  if (!gridView_.isBackTileSelected()) {
    entries.insert(
      entries.begin(),
      MenuRegistryEntry{
        .icon = {40, 40, Bookalt40Icon},
        .name = tr(STR_SELECTED_BOOK),
        .popupItems = {
          PopupMenuEntry{
            .label = tr(STR_ADD_TO_COLLECTION),
            .onSelected = [this]() { return openCollectionPicker(/*add=*/true); }
          },
          PopupMenuEntry{.label = tr(STR_REMOVE_FROM_COLLECTION), .onSelected = [this]() {
                           return openCollectionPicker(/*add=*/false);
                         }}
        }
      }
    );
  }

  return entries;
}
