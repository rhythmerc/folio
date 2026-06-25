#include "LibraryIndexingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <memory>
#include <string>

#include "LibraryIndex.h"
#include "activities/ActivityManager.h"
#include "activities/home/LibraryActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.generated.h"
#include "components/ui/ProgressBar/ProgressBar.h"
#include "util/Flex.h"

namespace {
constexpr char LOG_TAG[] = "LIDX";

int libFont(FontRole role) { return GUI.getFontForRole(role); }
}  // namespace

void LibraryIndexingActivity::onEnter() {
  Activity::onEnter();

  LIBRARY_INDEX.loadFromFile();

  // New EPUBs trigger the full-screen indexing progress view. The callback fires only
  // when there's work, so a warm cache never paints. Each repaint is a blocking ~400ms
  // e-ink refresh, so throttle to ~5% buckets (always painting the first book and 100%)
  // — ~20 refreshes over a cold index.
  int lastBucket = -1;
  LIBRARY_INDEX.refreshFromSdCard([this,
                                   &lastBucket](int done, int total, const char* label) {
    if (total <= 0) return;
    const int bucket = (done * 100 / total) / 5;
    if (done != 0 && done != total && bucket == lastBucket) return;
    lastBucket = bucket;
    renderIndexingProgress(done, total, label);
  });

  // Index is populated (and persists in the global singleton) — hand off to the library.
  // replaceActivity clears the stack so this transient screen doesn't linger on a back
  // path. Deferred to the next loop iteration, so onEnter returns first.
  activityManager.replaceActivity(
    std::make_unique<LibraryActivity>(renderer, mappedInput)
  );
}

void LibraryIndexingActivity::renderIndexingProgress(
  int done, int total, const char* label
) {
  renderer.clearScreen();

  const auto& td = *GUI.getData();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};

  // Header + body only — no button-hint band (indexing is non-interactive), so
  // the body grows to the bottom edge. (cf. UIPage::render, which reserves a
  // hint band we don't want here.)
  flex::Vstack page(
    screen,
    {
      flex::fixed(td.layout.topPadding),
      flex::fixed(td.header.height),
      flex::grow(),
    }
  );
  const auto top = flex::join(page[0], page[1]);
  renderer.fillRect(top.x, top.y, top.width, top.height, false);
  GUI.drawHeader(renderer, page[1], tr(STR_LIBRARY_INDEXING_TITLE), "", true);

  const Rect body = page[2];  // content is centered; no extra padding needed

  const int pct = total > 0 ? (done * 100) / total : 0;

  const int headFont = libFont(FontRole::Heading);
  const int pctFont = libFont(FontRole::Title);
  const int bodyFont = libFont(FontRole::Body);
  const int capFont = libFont(FontRole::CaptionCompact);

  const char* heading = tr(STR_LIBRARY_INDEXING_BUILDING);
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  char countBuf[48];
  snprintf(countBuf, sizeof(countBuf), "%d / %d %s", done, total, tr(STR_BOOKS));

  const int headH = renderer.getLineHeight(headFont);
  const int pctH = renderer.getLineHeight(pctFont);
  const int barH = ProgressBar::kIntrinsicHeight * 2;  // a touch heftier than tile bars
  const int countH = renderer.getLineHeight(bodyFont);
  const int labelH = renderer.getLineHeight(capFont);
  const int barW = (body.width * 7) / 10;

  // Centered block — heading / percentage / bar / count / current-book —
  // vertically centered in the body; each row horizontally centered via flex::center.
  constexpr int kRowGap = 14;
  const int blockH = headH + pctH + barH + countH + labelH + 4 * kRowGap;
  const Rect block = flex::center(body, body.width, blockH);
  flex::Vstack rows(
    block,
    {
      flex::fixed(headH),
      flex::fixed(pctH),
      flex::fixed(barH),
      flex::fixed(countH),
      flex::fixed(labelH),
    },
    kRowGap
  );

  {
    const Rect r = flex::center(rows[0], renderer.getTextWidth(headFont, heading), headH);
    renderer.drawText(headFont, r.x, r.y, heading, true);
  }
  {
    const Rect r = flex::center(rows[1], renderer.getTextWidth(pctFont, pctBuf), pctH);
    renderer.drawText(pctFont, r.x, r.y, pctBuf, true);
  }
  {
    const Rect r = flex::center(rows[2], barW, barH);
    ProgressBar::render(renderer, r, pct, true);
  }
  {
    const Rect r =
      flex::center(rows[3], renderer.getTextWidth(bodyFont, countBuf), countH);
    renderer.drawText(bodyFont, r.x, r.y, countBuf, true);
  }
  if (label != nullptr && label[0] != '\0') {
    const int width = (rows[4].width) * 7 / 10;
    const Rect r1 = flex::center(rows[4], width, labelH);

    std::string truncatedLabel =
      renderer.truncatedText(capFont, label, width, EpdFontFamily::ITALIC);
    const int truncatedWidth = renderer.getTextWidth(capFont, truncatedLabel.c_str());

    const Rect r2 = flex::center(rows[4], truncatedWidth, labelH);

    renderer.drawText(
      capFont, r2.x, r2.y, truncatedLabel.c_str(), true, EpdFontFamily::ITALIC
    );
  }

  // One-time expectation-setter, ~1.5 button-hint heights above the screen bottom.
  {
    const char* note = tr(STR_LIBRARY_INDEXING_NOTE);
    const int noteY = screen.height - (3 * td.buttonHints.height) / 2 - labelH;
    const Rect noteRow{0, noteY, screen.width, labelH};
    const Rect r = flex::center(
      noteRow, renderer.getTextWidth(capFont, note, EpdFontFamily::ITALIC), labelH
    );
    renderer.drawText(capFont, r.x, r.y, note, true, EpdFontFamily::ITALIC);
  }

  renderer.displayBuffer();
}
