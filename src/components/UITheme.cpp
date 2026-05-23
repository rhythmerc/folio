#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdThemeLoader.h"
#include "ThemeFontRegistry.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload(GfxRenderer& renderer) {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType, renderer);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type, GfxRenderer& renderer) {
  if (type == CrossPointSettings::SD_THEME && SETTINGS.sdThemeName[0] != '\0') {
    if (SD_THEMES.loadTheme(SETTINGS.sdThemeName, renderer)) {
      const ThemeData* sdData = SD_THEMES.getData();
      if (!currentTheme) {
        currentTheme = std::make_unique<BaseTheme>(sdData);
      } else {
        currentTheme->setData(sdData);
      }
      THEME_FONTS.setActiveTheme(renderer, sdData->id);
      return;
    }
    LOG_ERR("UI", "SD theme '%s' failed to load; falling back to Folio", SETTINGS.sdThemeName);
  } else {
    SD_THEMES.unloadTheme(renderer);
  }

  setTheme(CrossPointSettings::FOLIO);
  THEME_FONTS.setActiveTheme(renderer, currentTheme->getData()->id);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  const ThemeData* selected = &BuiltinThemes::Folio;
  if (type == CrossPointSettings::SD_THEME && SD_THEMES.isLoaded()) {
    selected = SD_THEMES.getData();
  }
  if (!currentTheme) {
    currentTheme = std::make_unique<BaseTheme>(selected);
  } else {
    currentTheme->setData(selected);
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const auto& td = *UITheme::getInstance().getTheme().getData();
  auto orientation = renderer.getOrientation();
  int reservedHeight = td.layout.topPadding;
  if (hasHeader) reservedHeight += td.header.height + td.layout.verticalSpacing;
  if (hasTabBar) reservedHeight += td.tabBar.height;
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += td.layout.verticalSpacing + td.buttonHints.height;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  const int rowHeight = hasSubtitle ? td.list.rowHeightWithSubtitle : td.list.rowHeight;
  return availableHeight / rowHeight;
}

Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool /*hasSideButtonHints*/) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const auto& td = *currentTheme->getData();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) safeArea.height -= td.buttonHints.height;
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += td.buttonHints.height;
        safeArea.width -= td.buttonHints.height;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += td.buttonHints.height;
        safeArea.height -= td.buttonHints.height;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) safeArea.width -= td.buttonHints.height;
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') return Folder;
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) return Book;
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) return Text;
  if (FsHelpers::hasBmpExtension(filename)) return Image;
  return File;
}

int UITheme::getStatusBarHeight() {
  const auto& td = *getInstance().getTheme().getData();
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (td.statusBar.verticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + td.progressBar.marginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const auto& td = *getInstance().getTheme().getData();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + td.progressBar.marginTop) : 0);
}

void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}
