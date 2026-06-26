#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryGridView.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect

// The Library shell: a thin coordinator around a LibraryGridView. It owns the page
// scaffold (header, scroll indicator, empty state), resolves the active subset from
// settings (with the fallback policy), and hosts the sort/collections/search menu. All
// grid state, navigation, cover prefetching, and tile rendering live in the view.
class LibraryActivity final : public Activity {
 private:
  LibraryGridView gridView_{renderer, mappedInput};

  // ---- Collections / menu -------------------------------------------------
  // Launch the collection-membership picker for the selected book. Returns true (close
  // the popup) when there's no book to edit; false while the picker is launched.
  bool openCollectionPicker(bool add);

  // Confirm: open the selected book, or return to All Books from the back tile.
  void doSelect();
  // Launch the keyboard for a search query; on confirm switch to the SEARCH view.
  bool onSearch();
  // After the membership picker returns: rebuild a collection view (its contents
  // changed).
  void onCollectionMembershipChanged();
  // After CollectionsActivity returns: rebuild only when the active view changed.
  void onReturnFromCollections(bool viewChanged);
  // Re-resolve the active subset and reset selection to the first book.
  void reloadActiveView();
  // Switch back to All Books (the back tile), persist, repaint.
  void activateBack();
  // Re-sort the index from settings and reset selection to the top.
  void applySort();
  void setSort(uint8_t field, uint8_t direction);
  bool onSortSelect(CrossPointSettings::LIBRARY_SORT_FIELD sortType);
  std::optional<PopupMenu::Glyph> getSortGlyph(
    CrossPointSettings::LIBRARY_SORT_FIELD sortType
  );

  // ---- Subset resolution --------------------------------------------------
  // Resolve the persisted active view (kind + collection id / name) into a subset for
  // the grid view via LibrarySubsetManager, applying the fallback policy (a stale
  // collection or an emptied auto-group resets to All Books and resaves settings).
  LibraryGridView::Subset resolveSubset();

  // ---- Render scaffold ----------------------------------------------------
  void renderPasses();
  void renderBattery(const Rect& headerBox);
  std::string getHeaderSubtitleText();
  // The shelf content rect: the page scaffold down to the body/rail split. Sizes the
  // grid view's prefetcher and is the area the view renders into. Mirrors the render
  // path's scaffold (UIPage + body/rail Hstack) without drawing — keep in sync.
  Rect computeContentRect();
  // The pagination indicator in the rail. Reads only page state (not the grid), so
  // swapping to a different scroll indicator later is a shell-only change.
  void renderScrollIndicator(
    const Rect& railArea, uint8_t currentPage, uint8_t pageCount
  );
  void renderEmptyState(const Rect& body);

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Power-button override: short-press advances through the library (next book / next in
  // row, per settings).
  bool handlePowerShortPress() override;

  std::vector<MenuRegistryEntry> getGlobalMenuEntries() override;
  AppId appId() const override { return AppId::Library; }
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    return GlobalMenuConfig{.clearFontCacheOnClose = false};
  }
};
