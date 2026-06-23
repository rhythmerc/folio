#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SdCardFont.h"  // CPFONT_VERSION (theme-bundled fonts share the .cpfont format)
#include "activities/Activity.h"
#include "components/ui/List/List.h"
#include "util/ButtonNavigator.h"

// JSON schema version of themes.json. Canonical copy lives in
// scripts/cptheme_version.py; bump this manually when the firmware learns a new
// manifest schema.
#define THEME_MANIFEST_VERSION 1

#ifndef THEME_MANIFEST_URL
// Manifest + .cptheme assets are published by folio-themes' release-themes.yml
// to the "sd-themes-m<META>-b<BIN>" tag. The tag pattern must stay in sync with
// that workflow (META = THEME_MANIFEST_VERSION, BIN = CPFONT_VERSION).
#define THEME_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define THEME_MANIFEST_URL_STRINGIFY(x) THEME_MANIFEST_URL_STRINGIFY_INNER(x)
#define THEME_MANIFEST_URL                                       \
  "https://github.com/folio-etc/folio-themes/releases/download/" \
  "sd-themes-m" THEME_MANIFEST_URL_STRINGIFY(                    \
    THEME_MANIFEST_VERSION                                       \
  ) "-b" THEME_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/themes.json"
#endif

// Downloads .cptheme archives from the folio-themes release into /.themes/ and
// rediscovers them so they appear in the UI Theme picker. Mirrors
// FontDownloadActivity, but a theme is a single file (no per-style directory),
// so there is no installer class — install is download + CRC + discoverThemes.
class ThemeDownloadActivity : public Activity {
 public:
  explicit ThemeDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    // HttpDownloader is blocking, so the main loop can't poll preventAutoSleep
    // mid-download — keep awake across every non-idle state, as fonts do.
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING || state_ == COMPLETE ||
           state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    THEME_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // pfrjson schema — field names must match themes.json keys exactly.
  struct ManifestTheme {
    std::string id;
    std::string name;
    std::string description;
    std::string file;
    uint32_t size = 0;
    uint32_t crc32 = 0;
  };

  // Manifest entry + runtime-only install state (kept out of the schema struct).
  struct ThemeEntry {
    ManifestTheme info;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;
  List list_;

  std::string baseUrl_;
  std::vector<ThemeEntry> themes_;

  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingIndex_ = -1;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void refreshInstallState();
  void buildList();
  void downloadTheme(int index);
  void promptDeleteTheme(int index);
  void onDeleteConfirmationResult(const ActivityResult& result, int index);
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  static void buildThemePath(const char* file, char* outBuf, size_t outBufSize);
};
