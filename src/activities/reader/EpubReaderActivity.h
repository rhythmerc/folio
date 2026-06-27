#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>

#include <optional>
#include <vector>

#include "../../BookmarkEntry.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  // Deserialized page held across re-renders of the same page (e.g. every menu
  // re-render) so we skip the ~100ms SD read. Invalidated on section rebuild.
  std::unique_ptr<Page> cachedPage;
  int cachedPageNumber = -1;  // section->currentPage cachedPage was filled for
  // Consecutive page-load failures. Bounds the rebuild retry so a corrupt
  // page can't spin render() forever. Reset to 0 on any successful load.
  int pageLoadRetries = 0;
  // Last position persisted by render()'s per-render progress save. Guards the
  // SD write so re-renders that don't move the page (e.g. every button press
  // with the global menu open) skip the ~tens-of-ms write. -1 forces a save.
  int savedSpineIndex = -1;
  int savedPageNumber = -1;
  int savedPageCount = -1;
  // True while the GlobalMenu is open: tells image rendering to build/use a RAM
  // BW cache so menu re-renders don't re-stream the image from SD.
  bool bwImageCacheActive = false;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  // Page identity currentPageFootnotes was last filled for. render() reloads on
  // every refresh (incl. while the GlobalMenu is open), but the footnote menu
  // labels point into currentPageFootnotes, so it must stay put unless the page
  // changed. -1 forces a fill on first render.
  int footnotesSpineIndex = -1;
  int footnotesPage = -1;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Bookmark support. cachedBookmarks mirrors the on-SD bookmark file for this
  // book; currentPageBookmarked drives the status-bar indicator and the toggle
  // icon; bookmarkRemoved records which way the last toggle went.
  static constexpr size_t initialBookmarkCacheCapacity = 16;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;
  // Guards the Confirm long-press bookmark toggle so it fires once per hold.
  bool bookmarkLongPressFired = false;

  void renderContents(const Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Sidebar (GlobalMenu) integration. Tall orientations show every action at the
  // top level; wide (landscape) orientations fold the extras under a "More" entry
  // because the nav strip has far fewer 64px slots when the screen is 480px tall.
  std::vector<MenuRegistryEntry> buildTallMenuEntries();
  std::vector<MenuRegistryEntry> buildWideMenuEntries();
  std::vector<PopupMenuEntry> navigateItems();
  std::vector<PopupMenuEntry> orientationItems();
  std::vector<PopupMenuEntry> autoTurnItems();
  std::vector<PopupMenuEntry> toolItems();
  std::vector<PopupMenuEntry> footnoteItems();

  // Reader actions invoked from the sidebar (formerly onReaderMenuConfirm cases).
  void selectChapter();
  void goToPercent();
  void openBookmarks();

  // Bookmark subsystem.
  CrossPointPosition getCurrentPosition() const;
  void loadCachedBookmarks();
  // Toggle a bookmark at the current page: remove any that match, otherwise add one.
  void addBookmark();
  // Refresh currentPageBookmarked against cachedBookmarks for the current page.
  void updateBookmarkFlag();
  void displayQr();
  void takeScreenshot();
  void startSync();
  void deleteCacheAndExit();
  // Current auto-turn rate index (into PAGE_TURN_RATES); 0 = Off. Mirrors the
  // value last applied via toggleAutoPageTurn so the submenu can mark it.
  uint8_t autoTurnOption = 0;

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  void onGlobalMenuOpened() override { bwImageCacheActive = true; }
  void onGlobalMenuClosed() override {
    bwImageCacheActive = false;
    if (cachedPage) cachedPage->releaseImageCaches();
  }
  bool isReaderActivity() const override { return true; }
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    // clearFontCacheOnClose reclaims the UI-font RAM so the grayscale framebuffer snapshot has headroom.
    return GlobalMenuConfig{.clearFontCacheOnClose = true};
  }
  std::vector<MenuRegistryEntry> getGlobalMenuEntries() override;
  ScreenshotInfo getScreenshotInfo() const override;
};
