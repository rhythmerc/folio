#include "UiThemeLoader.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "ThemeFontManager.h"
#include "ThemeJsonParser.h"
#include "components/themes/ThemeData.h"

namespace {
constexpr char LOG_TAG[] = "UITHM";
}  // namespace

UiThemeLoader& UiThemeLoader::getInstance() {
  static UiThemeLoader instance;
  return instance;
}

UiThemeLoader::~UiThemeLoader() = default;

void UiThemeLoader::discoverThemes() { archive_.discoverThemes(); }

bool UiThemeLoader::loadTheme(const char* themeId, GfxRenderer& renderer) {
  if (!themeId || themeId[0] == '\0') return false;

  if (isLoaded() && strcmp(idBuf_, themeId) == 0) return true;

  const CpThemeArchive::ThemeInfo* info = archive_.findThemeInfo(themeId);
  if (!info) {
    LOG_ERR(LOG_TAG, "Theme '%s' not found in discovered list", themeId);
    return false;
  }

  if (!archive_.ensureExtracted(*info)) {
    LOG_ERR(LOG_TAG, "Failed to extract %s", info->cpthemePath);
    return false;
  }

  char jsonPath[192];
  archive_.getCachedJsonPath(themeId, jsonPath, sizeof(jsonPath));

  FsFile jsonFile;
  if (!Storage.openFileForRead(LOG_TAG, jsonPath, jsonFile)) {
    LOG_ERR(LOG_TAG, "Cannot read cached %s", jsonPath);
    return false;
  }

  const size_t jsonSize = jsonFile.fileSize();
  auto jsonBuf = makeUniqueNoThrow<char[]>(jsonSize + 1);
  if (!jsonBuf) {
    LOG_ERR(LOG_TAG, "OOM: %d bytes for theme JSON", static_cast<int>(jsonSize));
    return false;
  }

  const int bytesRead = jsonFile.read(jsonBuf.get(), jsonSize);
  if (bytesRead < 0 || static_cast<size_t>(bytesRead) != jsonSize) {
    LOG_ERR(LOG_TAG, "Short read on %s", jsonPath);
    return false;
  }
  jsonBuf[jsonSize] = '\0';

  ThemeFontManager::getInstance().clear(renderer);

  auto newData = makeUniqueNoThrow<ThemeData>();
  if (!newData) {
    LOG_ERR(LOG_TAG, "OOM: ThemeData");
    return false;
  }

  ThemeFontSpec fontSpec;
  if (!parseThemeJson(jsonBuf.get(), jsonSize, *newData, idBuf_, sizeof(idBuf_),
                      nameBuf_, sizeof(nameBuf_), fontSpec)) {
    LOG_ERR(LOG_TAG, "Failed to parse theme.json for '%s'", themeId);
    return false;
  }

  newData->id = idBuf_;
  themeData_ = std::move(newData);

  char cacheDir[128];
  archive_.getCacheDir(themeId, cacheDir, sizeof(cacheDir));
  const auto ids = ThemeFontManager::getInstance().loadRoles(themeId, cacheDir, fontSpec, renderer);
  themeData_->fonts.titleId = ids.title;
  themeData_->fonts.headingId = ids.heading;
  themeData_->fonts.bodyId = ids.body;
  themeData_->fonts.captionId = ids.caption;
  themeData_->fonts.accentId = ids.accent;
  themeData_->fonts.bodyIdCompact = ids.bodyCompact;
  themeData_->fonts.captionIdCompact = ids.captionCompact;
  themeData_->fonts.accentIdCompact = ids.accentCompact;

  LOG_INF(LOG_TAG, "Loaded SD theme '%s' (%s)", nameBuf_, idBuf_);
  return true;
}

void UiThemeLoader::unloadTheme(GfxRenderer& renderer) {
  ThemeFontManager::getInstance().clear(renderer);
  themeData_.reset();
  idBuf_[0] = '\0';
  nameBuf_[0] = '\0';
}
