#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  // Page index currentPageLines was wrapped for (-1 = none). Lets render() skip the
  // SD reload + progress save when the page is unchanged (e.g. GlobalMenu re-composites).
  int loadedPageLinesFor = -1;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(
    size_t offset, std::vector<std::string>& outLines, size_t& nextOffset
  );
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt
  )
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // TXT has no reader-specific menu actions; the shared bottom row (home / file browser /
  // settings) is enough. Opting in just enables the GlobalMenu on Back.
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    return GlobalMenuConfig{};
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
