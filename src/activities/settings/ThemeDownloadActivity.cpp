#include "ThemeDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>
#include <esp_wifi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "UiThemeLoader.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/ui/UIPage/UIPage.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "stores/util/PfrJson.h"

namespace {
constexpr const char* THEMES_DIR = "/.themes";
constexpr const char* MANIFEST_TMP = "/themes_manifest.tmp";
}  // namespace

ThemeDownloadActivity::ThemeDownloadActivity(
  GfxRenderer& renderer, MappedInputManager& mappedInput
)
    : Activity("ThemeDownload", renderer, mappedInput) {}

// --- Lifecycle ---

void ThemeDownloadActivity::onEnter() {
  Activity::onEnter();

  WiFi.mode(WIFI_STA);
  startActivityForResult(
    std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
    [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); }
  );
}

void ThemeDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // restore default power-save (parity with OtaUpdater)
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ThemeDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  // Modem power-save throttles TCP throughput hard; disable it for the download
  // (restored in onExit). Same trick as OtaUpdater.
  esp_wifi_set_ps(WIFI_PS_NONE);

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = THEME_LIST;
  }
}

// --- Manifest fetching ---

bool ThemeDownloadActivity::fetchAndParseManifest() {
  // Free up heap by clearing font cache. Re-warms on render. 
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  if (
    HttpDownloader::downloadToFile(THEME_MANIFEST_URL, MANIFEST_TMP, nullptr) !=
    HttpDownloader::OK
  ) {
    LOG_ERR("THEME", "Failed to fetch manifest from %s", THEME_MANIFEST_URL);
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // Reuse the nested ManifestTheme as the pfrjson row schema.
  struct ThemeManifest {
    uint32_t version = 0;
    std::string baseUrl;
    std::vector<ManifestTheme> themes;
  };

  ThemeManifest manifest;
  {
    FsFile manifestFile;
    if (!Storage.openFileForRead("THEME", MANIFEST_TMP, manifestFile)) {
      LOG_ERR("THEME", "Failed to open temp manifest");
      Storage.remove(MANIFEST_TMP);
      errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
      return false;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, manifestFile);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);

    if (err) {
      LOG_ERR("THEME", "Manifest parse error: %s", err.c_str());
      errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
      return false;
    }
    pfrjson::read(doc.as<JsonVariantConst>(), manifest);
  }

  if (manifest.version != THEME_MANIFEST_VERSION) {
    LOG_ERR("THEME", "Unsupported manifest version: %u", manifest.version);
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    return false;
  }

  baseUrl_ = manifest.baseUrl;
  themes_.clear();
  themes_.reserve(manifest.themes.size());
  for (auto& m : manifest.themes) {
    if (m.id.empty() || m.file.empty()) continue;
    ThemeEntry e;
    e.info = std::move(m);
    themes_.push_back(std::move(e));
  }

  refreshInstallState();
  buildList();
  LOG_DBG("THEME", "Manifest loaded: %zu themes", themes_.size());
  return true;
}

void ThemeDownloadActivity::refreshInstallState() {
  const auto& discovered = UI_THEMES.getDiscoveredThemes();
  for (auto& e : themes_) {
    e.installed = false;
    for (const auto& d : discovered) {
      if (e.info.id == d.id) {
        e.installed = true;
        break;
      }
    }

    e.hasUpdate = false;
    if (e.installed) {
      char path[160];
      buildThemePath(e.info.file.c_str(), path, sizeof(path));
      FsFile f;
      if (Storage.openFileForRead("THEME", path, f)) {
        const size_t actual = f.fileSize();
        f.close();
        if (actual != e.info.size) e.hasUpdate = true;
      } else {
        // Discovered under a different path (e.g. manual /themes copy); leave
        // installed, no update prompt — re-download would still land in /.themes.
      }
    }
  }
}

// --- List building ---

void ThemeDownloadActivity::buildList() {
  const int saved = list_.selectedIndex();

  std::vector<ListItem> items;
  items.reserve(themes_.size());
  for (const auto& e : themes_) {
    ListItem item;
    item.title = e.info.name;
    item.subtitle = e.info.description;
    item.value =
      e.hasUpdate ? tr(STR_UPDATE_AVAILABLE) : (e.installed ? tr(STR_INSTALLED) : "");
    items.push_back(std::move(item));
  }
  list_.valueMetaStyle = true;
  list_.setItems(std::move(items));
  list_.setSelectedIndex(saved);
}

// --- Download / delete ---

void ThemeDownloadActivity::buildThemePath(
  const char* file, char* outBuf, size_t outBufSize
) {
  snprintf(outBuf, outBufSize, "%s/%s", THEMES_DIR, file);
}

void ThemeDownloadActivity::downloadTheme(int index) {
  if (index < 0 || index >= static_cast<int>(themes_.size())) return;

  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingIndex_ = index;
    fileProgress_ = 0;
    fileTotal_ = themes_[index].info.size;
    cancelRequested_ = false;
  }

  requestUpdateAndWait();

  if (!Storage.exists(THEMES_DIR) && !Storage.mkdir(THEMES_DIR)) {
    LOG_ERR("THEME", "Failed to create %s", THEMES_DIR);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    return;
  }

  const auto& info = themes_[index].info;
  char destPath[160];
  buildThemePath(info.file.c_str(), destPath, sizeof(destPath));
  const std::string url = baseUrl_ + info.file;

  // Reclaim the UI-font glyph cache for transfer headroom; self-rewarms on the next render.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  const auto result = HttpDownloader::downloadToFile(
    url,
    destPath,
    [this](size_t downloaded, size_t total) {
      fileProgress_ = downloaded;
      fileTotal_ = total;

      mappedInput.update();
      if (
        mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)
      ) {
        cancelRequested_ = true;
      }
      requestUpdate(true);
    },
    &cancelRequested_
  );

  if (result == HttpDownloader::ABORTED) {
    Storage.remove(destPath);
    RenderLock lock(*this);
    state_ = THEME_LIST;
    return;
  }

  if (result != HttpDownloader::OK) {
    LOG_ERR("THEME", "Download failed: %s (%d)", info.file.c_str(), result);
    Storage.remove(destPath);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    return;
  }

  uint32_t actualCrc = 0;
  if (!computeFileCrc32(destPath, actualCrc) || actualCrc != info.crc32) {
    LOG_ERR(
      "THEME",
      "CRC32 check failed for %s (got %08x expected %08x)",
      info.file.c_str(),
      actualCrc,
      info.crc32
    );
    Storage.remove(destPath);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    return;
  }
  LOG_DBG(
    "THEME", "Downloaded %s (size=%u crc32=%08x)", info.file.c_str(), info.size, actualCrc
  );

  UI_THEMES.discoverThemes();
  refreshInstallState();
  buildList();

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void ThemeDownloadActivity::promptDeleteTheme(int index) {
  if (index < 0 || index >= static_cast<int>(themes_.size())) return;
  startActivityForResult(
    std::make_unique<ConfirmationActivity>(
      renderer, mappedInput, tr(STR_DELETE), themes_[index].info.name
    ),
    [this, index](const ActivityResult& result) {
      onDeleteConfirmationResult(result, index);
    }
  );
}

void ThemeDownloadActivity::onDeleteConfirmationResult(
  const ActivityResult& result, int index
) {
  if (result.isCancelled || index < 0 || index >= static_cast<int>(themes_.size())) {
    requestUpdate();
    return;
  }

  const std::string id = themes_[index].info.id;

  // Remove the archive at its ACTUAL discovered path — a theme may have been
  // installed under /themes/ (visible root) rather than /.themes/, so the
  // download-time /.themes/<file> guess can miss and remove() would fail.
  const char* path = nullptr;
  for (const auto& d : UI_THEMES.getDiscoveredThemes()) {
    if (id == d.id) {
      path = d.cpthemePath;
      break;
    }
  }

  if (!path || !Storage.remove(path)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_THEME_INSTALL_FAILED);
    requestUpdate();
    return;
  }

  // Drop the extracted cache dir (/.themes/<id>/) so a delete reclaims its
  // space; best-effort, absent when the theme was never loaded.
  char cacheDir[160];
  snprintf(cacheDir, sizeof(cacheDir), "/.themes/%s", id.c_str());
  if (Storage.exists(cacheDir)) Storage.removeDir(cacheDir);

  // If the deleted theme was active, fall back to Folio (the caller reloads on
  // exit). Mirrors FontInstaller clearing the active SD font on delete.
  if (SETTINGS.uiTheme == CrossPointSettings::SD_THEME && id == SETTINGS.sdThemeName) {
    SETTINGS.uiTheme = CrossPointSettings::FOLIO;
    SETTINGS.sdThemeName[0] = '\0';
    SETTINGS.saveToFile();
  }

  UI_THEMES.discoverThemes();
  refreshInstallState();
  buildList();
  requestUpdate();
}

bool ThemeDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  FsFile f;
  if (!Storage.openFileForRead("THEME", path, f)) return false;
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

// --- Input handling ---

void ThemeDownloadActivity::loop() {
  if (state_ == THEME_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (!themes_.empty()) {
      buttonNavigator_.onNext([this] {
        list_.down();
        requestUpdate();
      });
      buttonNavigator_.onPrevious([this] {
        list_.up();
        requestUpdate();
      });

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        const int idx = list_.selectedIndex();
        if (idx >= 0 && idx < static_cast<int>(themes_.size())) {
          const auto& e = themes_[idx];
          if (e.installed && !e.hasUpdate) {
            promptDeleteTheme(idx);
          } else {
            downloadTheme(idx);
            requestUpdateAndWait();
          }
        }
        return;
      }
    }
  } else if (state_ == COMPLETE) {
    if (
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)
    ) {
      {
        RenderLock lock(*this);
        state_ = THEME_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = THEME_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (
        downloadingIndex_ >= 0 && downloadingIndex_ < static_cast<int>(themes_.size())
      ) {
        downloadTheme(downloadingIndex_);
        requestUpdateAndWait();
      } else {
        RenderLock lock(*this);
        state_ = THEME_LIST;
      }
    }
  }
}

// --- Rendering ---

void ThemeDownloadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& td = *GUI.getData();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  if (state_ == THEME_LIST) {
    const int idx = list_.selectedIndex();
    const bool deletable = idx >= 0 && idx < static_cast<int>(themes_.size()) &&
                           themes_[idx].installed && !themes_[idx].hasUpdate;
    const auto labels = mappedInput.mapLabels(
      tr(STR_BACK),
      themes_.empty() ? "" : (deletable ? tr(STR_DELETE) : tr(STR_DOWNLOAD)),
      themes_.empty() ? "" : tr(STR_DIR_UP),
      themes_.empty() ? "" : tr(STR_DIR_DOWN)
    );
    const Rect body = UIPage::render(renderer, tr(STR_THEME_BROWSER), nullptr, labels);

    if (themes_.empty()) {
      const int y = body.y + (body.height - lineHeight) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_NO_THEMES_AVAILABLE));
    } else {
      list_.render(renderer, body);
    }
    renderer.displayBuffer();
    return;
  }

  // Non-list states share the page chrome with a centered status message.
  const auto labels =
    (state_ == DOWNLOADING) ? mappedInput.mapLabels(tr(STR_CANCEL), "", "", "")
    : (state_ == ERROR)     ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "")
                            : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  const Rect body = UIPage::render(renderer, tr(STR_THEME_BROWSER), nullptr, labels);

  if (state_ == LOADING_MANIFEST) {
    const int y = body.y + (body.height - lineHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_LOADING_THEME_LIST));
  } else if (state_ == DOWNLOADING) {
    const std::string status =
      std::string(tr(STR_DOWNLOADING)) + " " +
      (downloadingIndex_ >= 0 ? themes_[downloadingIndex_].info.name : "");
    const int statusY = body.y + body.height / 2 - lineHeight;
    renderer.drawCenteredText(UI_10_FONT_ID, statusY, status.c_str());

    const int pct =
      fileTotal_ > 0 ? static_cast<int>(100.0 * fileProgress_ / fileTotal_) : 0;
    const Rect bar{
      body.x + td.layout.contentSidePadding,
      body.y + body.height / 2 + td.layout.verticalSpacing,
      body.width - 2 * td.layout.contentSidePadding,
      td.progressBar.height
    };
    GUI.drawProgressBar(renderer, bar, pct, 100);
  } else if (state_ == COMPLETE) {
    const int y = body.y + (body.height - lineHeight) / 2;
    renderer.drawCenteredText(
      UI_10_FONT_ID, y, tr(STR_THEME_INSTALLED), true, EpdFontFamily::BOLD
    );
  } else if (state_ == ERROR) {
    const int y = body.y + (body.height - lineHeight) / 2;
    renderer.drawCenteredText(
      UI_10_FONT_ID,
      y - lineHeight,
      tr(STR_THEME_INSTALL_FAILED),
      true,
      EpdFontFamily::BOLD
    );
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(
        UI_10_FONT_ID, y + td.layout.verticalSpacing, errorMessage_.c_str()
      );
    }
  }

  renderer.displayBuffer();
}
