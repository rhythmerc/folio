#include "CpThemeArchive.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ThemeJsonParser.h"
#include "components/themes/ThemeData.h"

namespace {
constexpr char LOG_TAG[] = "CPTHARC";

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

}  // namespace

void CpThemeArchive::discoverThemes() {
  themeList_.clear();
  for (int i = 0; i < SCAN_ROOT_COUNT; ++i) {
    scanRoot(SCAN_ROOTS[i]);
  }
  LOG_DBG(LOG_TAG, "Discovered %d SD theme(s)", static_cast<int>(themeList_.size()));
}

void CpThemeArchive::scanRoot(const char* rootPath) {
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

const CpThemeArchive::ThemeInfo* CpThemeArchive::findThemeInfo(const char* themeId) const {
  for (const auto& info : themeList_) {
    if (strcmp(info.id, themeId) == 0) return &info;
  }
  return nullptr;
}

bool CpThemeArchive::ensureExtracted(const ThemeInfo& info) {
  if (isExtracted(info.id, info.cpthemePath)) return true;
  return extractCptheme(info.cpthemePath, info.id);
}

void CpThemeArchive::getCachedJsonPath(const char* themeId, char* outPath, size_t outSize) const {
  snprintf(outPath, outSize, "%s/%s/%s", CACHE_ROOT, themeId, THEME_JSON);
}

void CpThemeArchive::getCacheDir(const char* themeId, char* outPath, size_t outSize) const {
  snprintf(outPath, outSize, "%s/%s", CACHE_ROOT, themeId);
}

bool CpThemeArchive::isExtracted(const char* themeId, const char* cpthemePath) const {
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

bool CpThemeArchive::extractCptheme(const char* cpthemePath, const char* themeId) {
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
