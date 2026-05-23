#include "SdThemeLoader.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <ZipFile.h>

#include <cstring>

#include "ThemeJsonParser.h"
#include "components/themes/BaseTheme.h"

namespace {
constexpr char LOG_TAG[] = "SDTHM";

constexpr const char* SCAN_ROOTS[] = {"/.themes", "/themes"};
constexpr int SCAN_ROOT_COUNT = 2;

constexpr const char* CACHE_ROOT = "/.themes";
constexpr const char* EXTRACTED_MARKER = ".extracted";
constexpr const char* THEME_JSON = "theme.json";
constexpr const char* CPTHEME_EXT = ".cptheme";
constexpr size_t EXTRACT_CHUNK_SIZE = 4096;

bool hasCpthemeExtension(const char* filename) {
  const size_t len = strlen(filename);
  const size_t extLen = strlen(CPTHEME_EXT);
  if (len <= extLen) return false;
  return strcmp(filename + len - extLen, CPTHEME_EXT) == 0;
}

uint32_t fnvHash(const char* str) {
  uint32_t hash = 2166136261u;
  while (*str) {
    hash ^= static_cast<uint8_t>(*str++);
    hash *= 16777619u;
  }
  return hash;
}

int computeBundledFontId(const char* themeId, const char* roleName) {
  uint32_t hash = fnvHash(themeId);
  hash ^= 0x42;
  hash *= 16777619u;
  while (*roleName) {
    hash ^= static_cast<uint8_t>(*roleName++);
    hash *= 16777619u;
  }
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

}  // namespace

SdThemeLoader& SdThemeLoader::getInstance() {
  static SdThemeLoader instance;
  return instance;
}

SdThemeLoader::~SdThemeLoader() = default;

void SdThemeLoader::discoverThemes() {
  themeList_.clear();
  for (int i = 0; i < SCAN_ROOT_COUNT; ++i) {
    scanRoot(SCAN_ROOTS[i]);
  }
  LOG_DBG(LOG_TAG, "Discovered %d SD theme(s)", static_cast<int>(themeList_.size()));
}

void SdThemeLoader::scanRoot(const char* rootPath) {
  if (!Storage.exists(rootPath)) return;

  FsFile dir = Storage.open(rootPath);
  if (!dir || !dir.isDirectory()) return;
  dir.rewindDirectory();

  char fileName[128];
  for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(fileName, sizeof(fileName));
    if (fileName[0] == '.') continue;
    if (!hasCpthemeExtension(fileName)) continue;

    char cpthemePath[192];
    snprintf(cpthemePath, sizeof(cpthemePath), "%s/%s", rootPath, fileName);

    std::string pathStr(cpthemePath);
    ZipFile zip(pathStr);
    size_t jsonSize = 0;
    uint8_t* jsonBuf = zip.readFileToMemory(THEME_JSON, &jsonSize, true);
    if (!jsonBuf) {
      LOG_ERR(LOG_TAG, "No theme.json in %s", cpthemePath);
      continue;
    }

    ThemeData tempData;
    char id[32] = "";
    char name[48] = "";
    ThemeFontSpec fontSpec;
    const bool ok = parseThemeJson(reinterpret_cast<const char*>(jsonBuf), jsonSize,
                                   tempData, id, sizeof(id), name, sizeof(name), fontSpec);
    free(jsonBuf);

    if (!ok || id[0] == '\0') {
      LOG_ERR(LOG_TAG, "Invalid theme.json in %s", cpthemePath);
      continue;
    }

    bool duplicate = false;
    for (const auto& existing : themeList_) {
      if (strcmp(existing.id, id) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      LOG_DBG(LOG_TAG, "Skipping duplicate theme id '%s' from %s", id, cpthemePath);
      continue;
    }

    ThemeInfo info;
    strncpy(info.id, id, sizeof(info.id) - 1);
    info.id[sizeof(info.id) - 1] = '\0';
    strncpy(info.name, name, sizeof(info.name) - 1);
    info.name[sizeof(info.name) - 1] = '\0';
    strncpy(info.cpthemePath, cpthemePath, sizeof(info.cpthemePath) - 1);
    info.cpthemePath[sizeof(info.cpthemePath) - 1] = '\0';

    themeList_.push_back(info);
    LOG_DBG(LOG_TAG, "Found theme '%s' (%s) at %s", name, id, cpthemePath);
  }
}

bool SdThemeLoader::loadTheme(const char* themeId, GfxRenderer& renderer) {
  if (!themeId || themeId[0] == '\0') return false;

  if (isLoaded() && strcmp(idBuf_, themeId) == 0) return true;

  const ThemeInfo* info = findThemeInfo(themeId);
  if (!info) {
    LOG_ERR(LOG_TAG, "Theme '%s' not found in discovered list", themeId);
    return false;
  }

  if (!isExtracted(themeId)) {
    if (!extractCptheme(info->cpthemePath, themeId)) {
      LOG_ERR(LOG_TAG, "Failed to extract %s", info->cpthemePath);
      return false;
    }
  }

  char jsonPath[192];
  snprintf(jsonPath, sizeof(jsonPath), "%s/%s/%s", CACHE_ROOT, themeId, THEME_JSON);

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

  clearFonts(renderer);

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

  loadThemeFonts(themeId, fontSpec, renderer);

  LOG_INF(LOG_TAG, "Loaded SD theme '%s' (%s)", nameBuf_, idBuf_);
  return true;
}

void SdThemeLoader::unloadTheme(GfxRenderer& renderer) {
  clearFonts(renderer);
  themeData_.reset();
  idBuf_[0] = '\0';
  nameBuf_[0] = '\0';
}

bool SdThemeLoader::isExtracted(const char* themeId) const {
  char markerPath[192];
  snprintf(markerPath, sizeof(markerPath), "%s/%s/%s", CACHE_ROOT, themeId, EXTRACTED_MARKER);
  return Storage.exists(markerPath);
}

bool SdThemeLoader::extractCptheme(const char* cpthemePath, const char* themeId) {
  LOG_INF(LOG_TAG, "Extracting %s", cpthemePath);

  char cacheDir[128];
  snprintf(cacheDir, sizeof(cacheDir), "%s/%s", CACHE_ROOT, themeId);
  if (!Storage.ensureDirectoryExists(cacheDir)) {
    LOG_ERR(LOG_TAG, "Cannot create cache dir %s", cacheDir);
    return false;
  }

  std::string pathStr(cpthemePath);
  ZipFile zip(pathStr);
  if (!zip.open()) {
    LOG_ERR(LOG_TAG, "Cannot open zip %s", cpthemePath);
    return false;
  }
  if (!zip.loadAllFileStatSlims()) {
    LOG_ERR(LOG_TAG, "Cannot read zip directory %s", cpthemePath);
    zip.close();
    return false;
  }
  zip.close();

  // Extract theme.json first.
  {
    char outPath[192];
    snprintf(outPath, sizeof(outPath), "%s/%s", cacheDir, THEME_JSON);
    FsFile outFile;
    if (!Storage.openFileForWrite(LOG_TAG, outPath, outFile)) {
      LOG_ERR(LOG_TAG, "Cannot create %s", outPath);
      return false;
    }
    if (!zip.readFileToStream(THEME_JSON, outFile, EXTRACT_CHUNK_SIZE)) {
      LOG_ERR(LOG_TAG, "Failed to extract %s", THEME_JSON);
      return false;
    }
  }

  // Read the just-extracted theme.json to discover font file paths.
  char jsonPath[192];
  snprintf(jsonPath, sizeof(jsonPath), "%s/%s", cacheDir, THEME_JSON);

  FsFile jsonFile;
  if (!Storage.openFileForRead(LOG_TAG, jsonPath, jsonFile)) {
    LOG_ERR(LOG_TAG, "Cannot read extracted %s", jsonPath);
    return false;
  }

  const size_t jsonSize = jsonFile.fileSize();
  auto jsonBuf = makeUniqueNoThrow<char[]>(jsonSize + 1);
  if (!jsonBuf) {
    LOG_ERR(LOG_TAG, "OOM: %d bytes", static_cast<int>(jsonSize));
    return false;
  }

  jsonFile.read(jsonBuf.get(), jsonSize);
  jsonBuf[jsonSize] = '\0';

  ThemeData tempData;
  char id[32], name[48];
  ThemeFontSpec fontSpec;
  if (!parseThemeJson(jsonBuf.get(), jsonSize, tempData, id, sizeof(id),
                      name, sizeof(name), fontSpec)) {
    LOG_ERR(LOG_TAG, "Cannot parse extracted theme.json");
    return false;
  }

  // Extract each referenced font file.
  for (int i = 0; i < kFontRoleCount; ++i) {
    if (fontSpec.roles[i].file[0] == '\0') continue;

    const char* zipPath = fontSpec.roles[i].file;
    char outPath[256];
    snprintf(outPath, sizeof(outPath), "%s/%s", cacheDir, zipPath);

    // Ensure parent directory exists (e.g. "fonts/").
    char parentDir[256];
    strncpy(parentDir, outPath, sizeof(parentDir) - 1);
    parentDir[sizeof(parentDir) - 1] = '\0';
    char* lastSlash = strrchr(parentDir, '/');
    if (lastSlash && lastSlash != parentDir) {
      *lastSlash = '\0';
      Storage.ensureDirectoryExists(parentDir);
    }

    FsFile outFile;
    if (!Storage.openFileForWrite(LOG_TAG, outPath, outFile)) {
      LOG_ERR(LOG_TAG, "Cannot create %s", outPath);
      continue;
    }
    if (!zip.readFileToStream(zipPath, outFile, EXTRACT_CHUNK_SIZE)) {
      LOG_ERR(LOG_TAG, "Failed to extract font %s", zipPath);
      continue;
    }
    LOG_DBG(LOG_TAG, "Extracted %s", outPath);
  }

  // Write the extraction marker last — incomplete extraction will be
  // retried on next load.
  char markerPath[192];
  snprintf(markerPath, sizeof(markerPath), "%s/%s/%s", CACHE_ROOT, themeId, EXTRACTED_MARKER);
  FsFile marker;
  if (Storage.openFileForWrite(LOG_TAG, markerPath, marker)) {
    marker.write(static_cast<uint8_t>('1'));
  }

  LOG_INF(LOG_TAG, "Extraction complete for '%s'", themeId);
  return true;
}

void SdThemeLoader::loadThemeFonts(const char* themeId, const ThemeFontSpec& fontSpec,
                                   GfxRenderer& renderer) {
  if (!themeData_) return;

  static constexpr const char* kRoleNames[] = {
      "title", "heading", "body", "caption", "accent",
      "body-compact", "caption-compact", "accent-compact"};

  int* fontIdSlots[] = {
      &themeData_->fonts.titleId,   &themeData_->fonts.headingId,
      &themeData_->fonts.bodyId,    &themeData_->fonts.captionId,
      &themeData_->fonts.accentId,  &themeData_->fonts.bodyIdCompact,
      &themeData_->fonts.captionIdCompact, &themeData_->fonts.accentIdCompact};

  for (int i = 0; i < kFontRoleCount; ++i) {
    const auto& role = fontSpec.roles[i];

    // Try loading an SD card font file first.
    if (role.file[0] != '\0') {
      char fontPath[256];
      snprintf(fontPath, sizeof(fontPath), "%s/%s/%s", CACHE_ROOT, themeId, role.file);

      if (Storage.exists(fontPath)) {
        auto font = std::make_unique<SdCardFont>();
        if (font->load(fontPath)) {
          const int fontId = computeBundledFontId(themeId, kRoleNames[i]);
          renderer.registerSdCardFont(fontId, font.get());
          EpdFontFamily family(font->getEpdFont(0), font->getEpdFont(1),
                               font->getEpdFont(2), font->getEpdFont(3));
          renderer.insertFont(fontId, family);

          *fontIdSlots[i] = fontId;
          loadedFonts_.push_back({fontId, std::move(font)});
          LOG_DBG(LOG_TAG, "Loaded bundled font %s -> id=%d", fontPath, fontId);
          continue;
        }
        LOG_ERR(LOG_TAG, "Failed to load font %s", fontPath);
      }
    }

    // Fall back to a builtin font reference.
    if (role.builtin[0] != '\0') {
      const int builtinId = resolveBuiltinFontName(role.builtin);
      if (builtinId != 0) {
        *fontIdSlots[i] = builtinId;
      }
    }
  }
}

void SdThemeLoader::clearFonts(GfxRenderer& renderer) {
  for (auto& lf : loadedFonts_) {
    renderer.removeFont(lf.fontId);
  }
  loadedFonts_.clear();
}

const SdThemeLoader::ThemeInfo* SdThemeLoader::findThemeInfo(const char* themeId) const {
  for (const auto& info : themeList_) {
    if (strcmp(info.id, themeId) == 0) return &info;
  }
  return nullptr;
}
