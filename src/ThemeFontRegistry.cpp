#include "ThemeFontRegistry.h"

#include <EpdFontFamily.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

namespace {
constexpr char LOG_TAG[] = "TFR";

// Two roots — mirrors the existing SD font convention (see
// SdCardFontRegistry::FONTS_DIR_HIDDEN / _VISIBLE). The "themes" subfolder
// keeps role-fonts separate from user-installed reader fonts so the existing
// reader font picker doesn't list them.
constexpr const char* THEMES_DIR_HIDDEN = "/.fonts/themes";
constexpr const char* THEMES_DIR_VISIBLE = "/fonts/themes";

constexpr char CPFONT_EXT[] = ".cpfont";

}  // namespace

ThemeFontRegistry& ThemeFontRegistry::getInstance() {
  static ThemeFontRegistry instance;
  return instance;
}

ThemeFontRegistry::~ThemeFontRegistry() = default;

void ThemeFontRegistry::discover(GfxRenderer& renderer) {
  clear(renderer);
  scanRoot(renderer, THEMES_DIR_HIDDEN);
  scanRoot(renderer, THEMES_DIR_VISIBLE);
  LOG_DBG(LOG_TAG, "Discovered %d theme role font(s)", static_cast<int>(loaded_.size()));
}

void ThemeFontRegistry::scanRoot(GfxRenderer& renderer, const char* rootPath) {
  if (!Storage.exists(rootPath)) return;

  HalFile root = Storage.open(rootPath);
  if (!root || !root.isDirectory()) {
    return;
  }
  root.rewindDirectory();

  // Each child of the root is expected to be a theme directory.
  char nameBuf[128];
  for (HalFile themeDir = root.openNextFile(); themeDir; themeDir = root.openNextFile()) {
    if (!themeDir.isDirectory()) continue;
    themeDir.getName(nameBuf, sizeof(nameBuf));
    if (nameBuf[0] == '.') continue;
    const std::string themeName{nameBuf};
    const std::string themeDirPath = std::string(rootPath) + "/" + themeName;

    themeDir.rewindDirectory();
    char roleFileName[128];
    for (HalFile roleFile = themeDir.openNextFile(); roleFile; roleFile = themeDir.openNextFile()) {
      if (roleFile.isDirectory()) continue;
      roleFile.getName(roleFileName, sizeof(roleFileName));
      if (roleFileName[0] == '.') continue;
      if (!FsHelpers::checkFileExtension(std::string_view{roleFileName}, CPFONT_EXT)) continue;

      // Strip ".cpfont" to recover the role name.
      const size_t nameLen = std::strlen(roleFileName);
      const size_t extLen = std::strlen(CPFONT_EXT);
      if (nameLen <= extLen) continue;
      const std::string roleName{roleFileName, nameLen - extLen};

      const std::string filePath = themeDirPath + "/" + roleFileName;
      loadRoleFile(renderer, themeName, roleName, filePath);
    }
    themeDir.close();
  }
  root.close();
}

bool ThemeFontRegistry::loadRoleFile(GfxRenderer& renderer, const std::string& themeName,
                                     const std::string& roleName, const std::string& filePath) {
  bool ok = false;
  const FontRole role = parseRoleName(roleName.c_str(), ok);
  if (!ok) {
    LOG_DBG(LOG_TAG, "Skipping %s: unrecognized role '%s'", filePath.c_str(), roleName.c_str());
    return false;
  }

  auto font = std::make_unique<SdCardFont>();
  if (!font->load(filePath.c_str())) {
    LOG_ERR(LOG_TAG, "Failed to load theme role font %s", filePath.c_str());
    return false;
  }

  const int fontId = computeFontId(font->contentHash(), themeName.c_str(), roleName.c_str());
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR(LOG_TAG, "Font ID %d collision; skipping %s", fontId, filePath.c_str());
    return false;
  }

  renderer.registerSdCardFont(fontId, font.get());
  EpdFontFamily family(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, family);

  LOG_DBG(LOG_TAG, "Loaded %s -> id=%d styles=%u", filePath.c_str(), fontId, font->styleCount());

  loaded_.push_back({themeName, role, fontId, std::move(font)});
  return true;
}

void ThemeFontRegistry::unloadAll(GfxRenderer& renderer) {
  clear(renderer);
  loaded_.shrink_to_fit();
}

int ThemeFontRegistry::getRoleFont(const char* themeName, FontRole role) const {
  if (themeName == nullptr) return 0;
  for (const auto& lf : loaded_) {
    if (lf.role == role && lf.themeName == themeName) return lf.fontId;
  }
  return 0;
}

void ThemeFontRegistry::clear(GfxRenderer& renderer) {
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
  }
  loaded_.clear();
}

FontRole ThemeFontRegistry::parseRoleName(const char* name, bool& ok) {
  ok = true;
  if (std::strcmp(name, "title") == 0) return FontRole::Title;
  if (std::strcmp(name, "heading") == 0) return FontRole::Heading;
  if (std::strcmp(name, "body") == 0) return FontRole::Body;
  if (std::strcmp(name, "caption") == 0) return FontRole::Caption;
  if (std::strcmp(name, "accent") == 0) return FontRole::Accent;
  ok = false;
  return FontRole::Body;
}

int ThemeFontRegistry::computeFontId(uint32_t contentHash, const char* themeName, const char* roleName) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*themeName) {
    hash ^= static_cast<uint8_t>(*themeName++);
    hash *= FNV_PRIME;
  }
  hash ^= 0x2E;  // '.' separator so role-X and themeX-role can't collide
  hash *= FNV_PRIME;
  while (*roleName) {
    hash ^= static_cast<uint8_t>(*roleName++);
    hash *= FNV_PRIME;
  }
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 reserved as "not found" sentinel
}
