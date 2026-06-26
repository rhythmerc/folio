#include "LibraryGridView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "components/UITheme.h"
#include "components/themes/ThemeData.generated.h"
#include "components/ui/Cover/Cover.h"
#include "components/ui/ProgressBar/ProgressBar.h"
#include "components/ui/TextBlock/TextBlock.h"
#include "util/Flex.h"

namespace {
int libFont(FontRole role) { return GUI.getFontForRole(role); }
}  // namespace

// ---- Lifecycle --------------------------------------------------------------

void LibraryGridView::onEnter(const Rect& content, Subset&& subset) {
  // First subset is set before the worker exists, so no lock is needed yet.
  subset_ = std::move(subset.books);
  subsetTitle_ = std::move(subset.title);
  hasBackTile_ = subset.hasBackTile;
  gridHelper = GridHelper(viewItemCount(), ROWS, COLS, hasBackTile_ ? 1 : 0);

  const Rect coverSlot = computeCoverSlot(content);
  prefetcher_.start(coverSlot.width, coverSlot.height);
  prefetcher_.loadPage(gridHelper.currentPage(), gridHelper.pageCount());
}

void LibraryGridView::onExit() { prefetcher_.stop(); }

void LibraryGridView::setSubset(
  const std::function<Subset()>& produce, InitialSelection sel
) {
  prefetcher_.cancelAll();
  {
    // Hold the cache lock across produce(): the worker reads subset_ / the index via the
    // resolver under it, and produce() may re-sort the index (invalidating subset_'s
    // pointers) before rebuilding subset_.
    auto g = prefetcher_.lockCache();
    Subset s = produce();
    subset_ = std::move(s.books);
    subsetTitle_ = std::move(s.title);
    hasBackTile_ = s.hasBackTile;
  }

  const int initialIndex = (sel == InitialSelection::Top) ? 0 : (hasBackTile_ ? 1 : 0);
  gridHelper = GridHelper(viewItemCount(), ROWS, COLS, initialIndex);
  prefetcher_.loadPage(0, gridHelper.pageCount());
}

// ---- Per-loop ---------------------------------------------------------------

bool LibraryGridView::pollCoverUpdates() { return prefetcher_.consumeBatchDone(); }

bool LibraryGridView::handleInput() {
  needsRepaint_ = false;

  if (
    rapidJumping_ && !mappedInput.isPressed(MappedInputManager::Button::Up) &&
    !mappedInput.isPressed(MappedInputManager::Button::Down)
  ) {
    endRapidJumpIfActive();
  }

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] { moveUp(); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] { moveDown(); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this] {
    jumpPageBack();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this] {
    jumpPageForward();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) moveLeft();
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) moveRight();

  return needsRepaint_;
}

// ---- Navigation -------------------------------------------------------------

void LibraryGridView::moveUp() {
  const uint8_t oldPage = gridHelper.currentPage();
  gridHelper.up();
  const uint8_t newPage = gridHelper.currentPage();
  // On a page change, swap the cache to the new page; the repaint paints placeholders
  // immediately and the worker's batch-done repaints with real covers. Within-page moves
  // just repaint (covers already cached).
  if (oldPage != newPage) prefetcher_.loadPage(newPage, gridHelper.pageCount());
  needsRepaint_ = true;
}

void LibraryGridView::moveDown() {
  const uint8_t oldPage = gridHelper.currentPage();
  gridHelper.down();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) prefetcher_.loadPage(newPage, gridHelper.pageCount());
  needsRepaint_ = true;
}

void LibraryGridView::moveLeft() {
  const uint8_t oldPage = gridHelper.currentPage();
  gridHelper.left();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) prefetcher_.loadPage(newPage, gridHelper.pageCount());
  needsRepaint_ = true;
}

void LibraryGridView::moveRight() {
  const uint8_t oldPage = gridHelper.currentPage();
  gridHelper.right();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) prefetcher_.loadPage(newPage, gridHelper.pageCount());
  needsRepaint_ = true;
}

void LibraryGridView::moveNext() {
  const uint8_t oldPage = gridHelper.currentPage();
  gridHelper.nextItem();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) prefetcher_.loadPage(newPage, gridHelper.pageCount());
  needsRepaint_ = true;
}

void LibraryGridView::jumpPageForward() {
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  // During a held rapid-jump, suppress prefetch / SD work — the render path takes the
  // placeholder fallback for uncached covers; prefetch resumes on release.
  if (!rapidJumping_) {
    rapidJumping_ = true;
    prefetcher_.cancelAll();
  }
  needsRepaint_ = true;
}

void LibraryGridView::jumpPageBack() {
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + pages - 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  if (!rapidJumping_) {
    rapidJumping_ = true;
    prefetcher_.cancelAll();
  }
  needsRepaint_ = true;
}

void LibraryGridView::endRapidJumpIfActive() {
  if (!rapidJumping_) return;
  rapidJumping_ = false;

  // Hold ended — swap the cache to the landed page; placeholders persist until the
  // worker's batch-done fills in real covers.
  prefetcher_.loadPage(gridHelper.currentPage(), gridHelper.pageCount());
  needsRepaint_ = true;
}

// ---- Active subset helpers --------------------------------------------------

int LibraryGridView::viewItemCount() const {
  return static_cast<int>(subset_.size()) + (hasBackTile_ ? 1 : 0);
}

bool LibraryGridView::isBackTileIndex(int gridIndex) const {
  return hasBackTile_ && gridIndex == 0;
}

const LibraryBook* LibraryGridView::bookForGridIndex(int gridIndex) const {
  if (gridIndex < 0) return nullptr;
  const int bookIdx = gridIndex - (hasBackTile_ ? 1 : 0);
  if (bookIdx < 0 || bookIdx >= static_cast<int>(subset_.size())) return nullptr;
  return subset_[bookIdx];
}

// ---- Render -----------------------------------------------------------------

Rect LibraryGridView::computeCoverSlot(const Rect& content) const {
  flex::Grid cells(content, ROWS, COLS, CELL_GAP_Y, CELL_GAP_X);
  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};
  flex::Vstack tile(
    cells[0],
    {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()},
    0,
    tilePad
  );
  return tile[0];
}

Rect LibraryGridView::computeSelectionFrame(const Rect& cell, int contentBottom) {
  // Floats from kSelectionLiftAbove above the cell top down to
  // kSelectionExtendBelowTileContent past `contentBottom`.
  const int top = cell.y - kSelectionLiftAbove;
  return Rect{
    cell.x - kSelectionInsetX,
    top,
    cell.width + kSelectionInsetX * 2,
    (contentBottom + kSelectionExtendBelowTileContent) - top,
  };
}

void LibraryGridView::renderBody(const Rect& content) {
  // Theme/orientation can change the cover-slot size between paints. When it does,
  // re-seed the draw envelope and reload the page at the new dims. We're on the render
  // task mid-paint: this frame paints placeholders for the just-cleared covers; the
  // worker's batch-done triggers the real repaint from the shell's loop().
  const Rect cs = computeCoverSlot(content);
  if (prefetcher_.setCoverBox(cs.width, cs.height)) {
    prefetcher_.loadPage(gridHelper.currentPage(), gridHelper.pageCount());
  }

  const uint8_t currentIndexOnPage = gridHelper.currentIndexOnPage();
  const uint8_t currentPg = gridHelper.currentPage();
  const uint8_t itemsPerPage = gridHelper.itemsPerPage();
  const uint16_t baseIndex = currentPg * itemsPerPage;

  flex::Grid cells(content, ROWS, COLS, CELL_GAP_Y, CELL_GAP_X);
  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const int idx = baseIndex + slot;
    if (isBackTileIndex(idx)) {
      renderBackTile(cells[slot], slot == currentIndexOnPage);
      continue;
    }
    const LibraryBook* book = bookForGridIndex(idx);
    if (book == nullptr) continue;
    renderTile(cells[slot], *book, slot == currentIndexOnPage);
  }
}

void LibraryGridView::renderTile(
  const Rect& cell, const LibraryBook& book, bool selected
) {
  const bool invertText = selected && GUI.getData()->selection.textInverted;
  const bool textBlack = !invertText;

  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  constexpr int kProgressGap = 8;

  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};

  Rect coverSlot;
  Rect textSlot;
  Rect progressSlot;

  if (book.hasProgress()) {
    flex::Vstack tile(
      cell,
      {flex::percent(kCoverPercent),
       flex::fixed(kCoverBottomPadding),
       flex::grow(),
       flex::fixed(kProgressGap),
       flex::fixed(ProgressBar::kIntrinsicHeight)},
      0,
      tilePad
    );
    coverSlot = tile[0];
    textSlot = tile[2];
    progressSlot = tile[4];
  } else {
    flex::Vstack tile(
      cell,
      {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()},
      0,
      tilePad
    );
    coverSlot = tile[0];
    textSlot = tile[2];
  }

  const int authorReserved = book.author.empty() ? 0 : (kTitleAuthorGap + captionLineH);
  const int titleBudget = textSlot.height - authorReserved;
  const int maxTitleLines = std::min(2, std::max(1, titleBudget / captionLineH));

  const std::vector<std::string> titleLines = renderer.wrappedText(
    captionFont,
    book.title.c_str(),
    textSlot.width - 8,
    maxTitleLines,
    EpdFontFamily::BOLD
  );

  const std::string authorTrunc =
    book.author.empty()
      ? std::string()
      : renderer.truncatedText(
          captionFont, book.author.c_str(), textSlot.width - 8, EpdFontFamily::ITALIC
        );

  const int textH = static_cast<int>(titleLines.size()) * captionLineH +
                    (authorTrunc.empty() ? 0 : (kTitleAuthorGap + captionLineH));

  const Rect textBox = flex::center(textSlot, textSlot.width, textH);

  const int contentBottom = book.hasProgress()
                              ? progressSlot.y + ProgressBar::kIntrinsicHeight
                              : textBox.y + textBox.height;

  Rect selectionRect{};
  if (selected) {
    selectionRect = computeSelectionFrame(cell, contentBottom);
    GUI.drawSelectionBackground(renderer, selectionRect);
  }

  {
    char thumbPath[64];
    CoverPrefetcher::thumbPath(book.pathHash, thumbPath, sizeof(thumbPath));

    {
      auto g = prefetcher_.lockCache();
      const Cover::Fallback coverFallback{
        book.title.c_str(), captionFont, EpdFontFamily::BOLD
      };
      Cover::render(
        renderer,
        coverSlot,
        prefetcher_.cache(),
        thumbPath,
        invertText,
        coverSlot.height * 2 / 3,
        coverSlot.height,
        &coverFallback
      );
    }

    TextBlock::Line tlines[3];  // up to 2 title lines + 1 author line
    std::size_t tcount = 0;
    for (const auto& l : titleLines) {
      tlines[tcount++] =
        TextBlock::Line{l.c_str(), captionFont, EpdFontFamily::BOLD, 0, false};
    }
    if (!authorTrunc.empty()) {
      tlines[tcount++] = TextBlock::Line{
        authorTrunc.c_str(), captionFont, EpdFontFamily::ITALIC, kTitleAuthorGap, false
      };
    }

    TextBlock::render(renderer, textBox, tlines, tcount, textBlack);

    if (book.hasProgress()) {
      const Rect barBox = flex::align(
        progressSlot,
        Cover::kFallbackWidth,
        ProgressBar::kIntrinsicHeight,
        flex::HAlign::Center,
        flex::VAlign::Start
      );
      ProgressBar::render(renderer, barBox, book.progressPercent(), textBlack);
    }
  }

  if (selected) {
    GUI.drawSelectionForeground(renderer, selectionRect);
  }
}

void LibraryGridView::renderBackTile(const Rect& cell, bool selected) {
  const bool invertText = selected && GUI.getData()->selection.textInverted;
  const bool textBlack = !invertText;

  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  const flex::Padding tilePad{kTilePadTop, 0, kTilePadBottom, 0};

  // Same geometry as a bar-less book tile so the back tile aligns with the grid.
  flex::Vstack tile(
    cell,
    {flex::percent(kCoverPercent), flex::fixed(kCoverBottomPadding), flex::grow()},
    0,
    tilePad
  );
  const Rect coverSlot = tile[0];
  const Rect textSlot = tile[2];

  const std::string labelTrunc = renderer.truncatedText(
    captionFont, tr(STR_ALL_BOOKS), textSlot.width - 8, EpdFontFamily::BOLD
  );
  const Rect textBox{textSlot.x, textSlot.y, textSlot.width, captionLineH};
  const int contentBottom = textBox.y + textBox.height;

  Rect selectionRect{};
  if (selected) {
    selectionRect = computeSelectionFrame(cell, contentBottom);
    GUI.drawSelectionBackground(renderer, selectionRect);
  }

  // A bordered box (fallback-cover sized) with a centered left arrow.
  const Rect box = flex::align(
    coverSlot,
    Cover::kFallbackWidth,
    Cover::kFallbackHeight,
    flex::HAlign::Center,
    flex::VAlign::Start
  );
  renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 2, 6, textBlack);
  const int cx = box.x + box.width / 2;
  const int cy = box.y + box.height / 2;
  const int arm = box.width / 4;
  const int barb = box.height / 8;
  renderer.drawLine(cx - arm, cy, cx + arm, cy, 3, textBlack);  // shaft
  renderer.drawLine(
    cx - arm, cy, cx - arm + barb, cy - barb, 3, textBlack
  );  // upper barb
  renderer.drawLine(
    cx - arm, cy, cx - arm + barb, cy + barb, 3, textBlack
  );  // lower barb

  TextBlock::Line line{labelTrunc.c_str(), captionFont, EpdFontFamily::BOLD, 0, false};
  TextBlock::render(renderer, textBox, &line, 1, textBlack);

  if (selected) {
    GUI.drawSelectionForeground(renderer, selectionRect);
  }
}
