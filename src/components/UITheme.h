#pragma once

#include <EpdFontFamily.h>

#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.h"

class UITheme {
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const BaseTheme& getTheme() const { return *currentTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Re-read the active theme from settings and apply it. The renderer-taking
  // overload also reconciles SD theme fonts (unloads previous theme's, loads
  // new theme's). Call the renderer overload from anywhere that knows the
  // theme might have changed since last apply (boot, settings exit).
  void reload();
  void reload(GfxRenderer& renderer);
  void setTheme(CrossPointSettings::UI_THEME type);
  void setTheme(CrossPointSettings::UI_THEME type, GfxRenderer& renderer);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  // Point the UI_12/UI_10 font ids at the active theme's Title/Body families so
  // UI chrome tracks the theme's fonts (lets us ship without a dedicated UI
  // font face). Called on every theme apply.
  void repointUiFonts(GfxRenderer& renderer);

  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
