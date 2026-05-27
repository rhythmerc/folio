#include "UiThemeLoader.h"

#include <Arduino.h>
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
constexpr char LOG_TAG[] = "UITHM";

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

uint32_t fnvHashBytes(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t hashFile(const char* path) {
  FsFile f;
  if (!Storage.openFileForRead(LOG_TAG, path, f)) return 0;
  const size_t size = f.fileSize();
  auto buf = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buf) return 0;
  f.read(buf.get(), size);
  return fnvHashBytes(buf.get(), size);
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

UiThemeLoader& UiThemeLoader::getInstance() {
  static UiThemeLoader instance;
  return instance;
}

UiThemeLoader::~UiThemeLoader() = default;

void UiThemeLoader::discoverThemes() {
  themeList_.clear();
  for (int i = 0; i < SCAN_ROOT_COUNT; ++i) {
    scanRoot(SCAN_ROOTS[i]);
  }
  LOG_DBG(LOG_TAG, "Discovered %d SD theme(s)", static_cast<int>(themeList_.size()));
}

void UiThemeLoader::scanRoot(const char* rootPath) {
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

bool UiThemeLoader::loadTheme(const char* themeId, GfxRenderer& renderer) {
  if (!themeId || themeId[0] == '\0') return false;

  if (isLoaded() && strcmp(idBuf_, themeId) == 0) return true;

  const ThemeInfo* info = findThemeInfo(themeId);
  if (!info) {
    LOG_ERR(LOG_TAG, "Theme '%s' not found in discovered list", themeId);
    return false;
  }

  if (!isExtracted(themeId, info->cpthemePath)) {
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

void UiThemeLoader::unloadTheme(GfxRenderer& renderer) {
  clearFonts(renderer);
  themeData_.reset();
  idBuf_[0] = '\0';
  nameBuf_[0] = '\0';
}

bool UiThemeLoader::isExtracted(const char* themeId, const char* cpthemePath) const {
  char markerPath[192];
  snprintf(markerPath, sizeof(markerPath), "%s/%s/%s", CACHE_ROOT, themeId, EXTRACTED_MARKER);
  FsFile marker;
  if (!Storage.openFileForRead(LOG_TAG, markerPath, marker)) return false;

  char stored[16] = "";
  const int n = marker.read(stored, sizeof(stored) - 1);
  if (n <= 0) return false;
  stored[n] = '\0';
  const uint32_t storedHash = strtoul(stored, nullptr, 16);

  const uint32_t currentHash = hashFile(cpthemePath);
  if (currentHash == 0) return false;
  return storedHash == currentHash;
}

bool UiThemeLoader::extractCptheme(const char* cpthemePath, const char* themeId) {
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
  // retried on next load. Store source file hash for invalidation.
  char markerPath[192];
  snprintf(markerPath, sizeof(markerPath), "%s/%s/%s", CACHE_ROOT, themeId, EXTRACTED_MARKER);
  FsFile marker;
  if (Storage.openFileForWrite(LOG_TAG, markerPath, marker)) {
    const uint32_t srcHash = hashFile(cpthemePath);
    char hashStr[12];
    snprintf(hashStr, sizeof(hashStr), "%08x", srcHash);
    marker.write(reinterpret_cast<const uint8_t*>(hashStr), strlen(hashStr));
  }

  LOG_INF(LOG_TAG, "Extraction complete for '%s'", themeId);
  return true;
}

void UiThemeLoader::loadThemeFonts(const char* themeId, const ThemeFontSpec& fontSpec,
                                   GfxRenderer& renderer) {
  if (!themeData_) return;

  LOG_INF(LOG_TAG, "loadThemeFonts: heap free=%u, maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

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
      char fontPath[128];
      snprintf(fontPath, sizeof(fontPath), "%s/%s/%s", CACHE_ROOT, themeId, role.file);

      if (Storage.exists(fontPath)) {
        // Reuse an already-loaded SdCardFont if another role pointed at the
        // same file. Each role still gets its own fontId so the renderer can
        // map role → font; only the heavy backing instance is shared.
        SdCardFont* sharedFont = nullptr;
        for (auto& lf : loadedFonts_) {
          if (strcmp(lf.path, fontPath) == 0) {
            sharedFont = lf.font.get();
            break;
          }
        }

        if (!sharedFont) {
          auto font = makeUniqueNoThrow<SdCardFont>();
          if (!font) {
            LOG_ERR(LOG_TAG, "OOM: SdCardFont for %s", fontPath);
          } else if (!font->load(fontPath)) {
            LOG_ERR(LOG_TAG, "Failed to load font %s", fontPath);
          } else {
            LoadedFont entry;
            strncpy(entry.path, fontPath, sizeof(entry.path) - 1);
            entry.path[sizeof(entry.path) - 1] = '\0';
            entry.font = std::move(font);
            sharedFont = entry.font.get();
            loadedFonts_.push_back(std::move(entry));
            LOG_INF(LOG_TAG, "Loaded %s (heap free=%u, maxAlloc=%u)", fontPath, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
          }
        }

        if (sharedFont) {
          const int fontId = computeBundledFontId(themeId, kRoleNames[i]);
          renderer.registerSdCardFont(fontId, sharedFont);
          // Resolve each slot through SdCardFont::resolveStyle() so missing
          // styles are backed by the closest present style instead of nullptr.
          // A .cpfont with only italic present (e.g. Nunito_7) would otherwise
          // hand a null `regular` pointer to EpdFontFamily, and the renderer
          // would deref it the first time any text drew in REGULAR style.
          EpdFontFamily family(sharedFont->getEpdFont(sharedFont->resolveStyle(0)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(1)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(2)),
                               sharedFont->getEpdFont(sharedFont->resolveStyle(3)));
          renderer.insertFont(fontId, family);

          *fontIdSlots[i] = fontId;
          registeredFontIds_.push_back(fontId);
          RoleEntry roleEntry;
          roleEntry.fontId = fontId;
          strncpy(roleEntry.path, fontPath, sizeof(roleEntry.path) - 1);
          roleEntry.path[sizeof(roleEntry.path) - 1] = '\0';
          roleEntries_.push_back(roleEntry);
          LOG_DBG(LOG_TAG, "Bound role '%s' -> %s (id=%d)", kRoleNames[i], fontPath, fontId);
          continue;
        }
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

void UiThemeLoader::clearFonts(GfxRenderer& renderer) {
  for (int fontId : registeredFontIds_) {
    renderer.removeFont(fontId);
  }
  registeredFontIds_.clear();
  loadedFonts_.clear();
  roleEntries_.clear();
}

void UiThemeLoader::evictFonts(GfxRenderer& renderer) {
  if (loadedFonts_.empty() && registeredFontIds_.empty()) return;
  LOG_DBG(LOG_TAG, "evictFonts: dropping %u SdCardFont(s), heap free=%u before",
          static_cast<unsigned>(loadedFonts_.size()), ESP.getFreeHeap());
  for (int fontId : registeredFontIds_) {
    renderer.removeFont(fontId);
  }
  registeredFontIds_.clear();
  loadedFonts_.clear();  // SdCardFont dtor calls freeAll() per font
  // roleEntries_ stays — tryLoadFontOnDemand needs the (fontId, path) records
  // to lazily reload roles on first use by the next activity.
  LOG_INF(LOG_TAG, "evictFonts: heap free=%u, maxAlloc=%u after", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

bool UiThemeLoader::tryLoadFontOnDemand(int fontId, GfxRenderer& renderer) {
  const RoleEntry* entry = nullptr;
  for (const auto& re : roleEntries_) {
    if (re.fontId == fontId) {
      entry = &re;
      break;
    }
  }
  if (!entry || entry->path[0] == '\0') return false;

  // Dedup against any role we already restored that points at the same file.
  SdCardFont* sharedFont = nullptr;
  for (auto& lf : loadedFonts_) {
    if (strcmp(lf.path, entry->path) == 0) {
      sharedFont = lf.font.get();
      break;
    }
  }

  if (!sharedFont) {
    auto font = makeUniqueNoThrow<SdCardFont>();
    if (!font) {
      LOG_ERR(LOG_TAG, "OOM: lazy SdCardFont for %s", entry->path);
      return false;
    }
    if (!font->load(entry->path)) {
      LOG_ERR(LOG_TAG, "Lazy load failed: %s", entry->path);
      return false;
    }
    LoadedFont lf;
    strncpy(lf.path, entry->path, sizeof(lf.path) - 1);
    lf.path[sizeof(lf.path) - 1] = '\0';
    lf.font = std::move(font);
    sharedFont = lf.font.get();
    loadedFonts_.push_back(std::move(lf));
    LOG_INF(LOG_TAG, "Lazy-loaded %s (heap free=%u, maxAlloc=%u)", entry->path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  renderer.registerSdCardFont(fontId, sharedFont);
  EpdFontFamily family(sharedFont->getEpdFont(sharedFont->resolveStyle(0)),
                       sharedFont->getEpdFont(sharedFont->resolveStyle(1)),
                       sharedFont->getEpdFont(sharedFont->resolveStyle(2)),
                       sharedFont->getEpdFont(sharedFont->resolveStyle(3)));
  renderer.insertFont(fontId, family);
  registeredFontIds_.push_back(fontId);
  return true;
}

bool UiThemeLoader::onFontMiss(int fontId, void* ctx) {
  auto* renderer = static_cast<GfxRenderer*>(ctx);
  if (!renderer) return false;
  return UiThemeLoader::getInstance().tryLoadFontOnDemand(fontId, *renderer);
}

const UiThemeLoader::ThemeInfo* UiThemeLoader::findThemeInfo(const char* themeId) const {
  for (const auto& info : themeList_) {
    if (strcmp(info.id, themeId) == 0) return &info;
  }
  return nullptr;
}
