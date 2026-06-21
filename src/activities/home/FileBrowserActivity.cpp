#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/ui/UIPage/UIPage.h"
#include "fontIds.h"
#include "components/icons/arrowUp40.h"
#include "components/icons/slash.h"
#include "components/icons/trashx40.h"
#include "components/icons/filepencil40.h"

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + GUI.getData()->layout.verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    this->onSelectEntry();

    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mode == Mode::PickFirmware) {
      // Firmware picker at root: cancel back to caller instead of going home.
      ActivityResult res;
      res.isCancelled = true;
      setResult(std::move(res));
      finish();
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& td = *GUI.getData();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));

  const auto labels = mappedInput.mapLabels(
      this->getGlobalMenuConfig().has_value() ? tr(STR_MENU_LABEL) : backLabel,
      confirmLabel, 
      files.empty() ? "" : tr(STR_DIR_UP),
      files.empty() ? "" : tr(STR_DIR_DOWN)
  );

  auto body = UIPage::render(
      renderer, 
      folderName.c_str(), 
      "", 
      labels,
      flex::Padding{},
      true
  );

  auto sections = flex::Vstack(body,
      {
        flex::grow(),
        flex::fixed(50)
      }
  );

  // draw section divider
  renderer.drawLine(0, sections[1].y, sections[1].width, sections[1].y, 2, true);

  auto listSection = flex::inset(sections[0], flex::xy(0, td.layout.topPadding));
  auto pathSection = flex::inset(sections[1], flex::xy(td.layout.contentSidePadding, 0));

  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);

    auto width = renderer.getTextWidth(UI_10_FONT_ID, emptyMsg);
    auto height = renderer.getLineHeight(UI_10_FONT_ID);
    auto position = flex::center(listSection, width, height);

    renderer.drawText(UI_10_FONT_ID, position.x, position.y, emptyMsg);
  } else {
    GUI.drawList(
        renderer, listSection, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const auto fontId = GUI.getFontForRole(FontRole::BodyCompact);

    const int pathMaxWidth = pathSection.width;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];

    if (renderer.getTextWidth(fontId, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(fontId, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(fontId, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }

    auto height = renderer.getLineHeight(fontId);
    auto position = flex::align(
        pathSection, 
        pathSection.width, 
        height, 
        flex::HAlign::Start, 
        flex::VAlign::Center
    );

    renderer.drawText(fontId, position.x, position.y, pathDisplay);
  }

  renderer.displayBuffer();
}

void FileBrowserActivity::onSelectEntry() {
  if (files.empty()) return;

  const std::string& entry = files[selectorIndex];
  bool isDirectory = (entry.back() == '/');

  // Firmware picker: select file -> return path; navigate into directories normally.
  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }


  if (basepath.back() != '/') basepath += "/";

  if (isDirectory) {
    basepath += entry.substr(0, entry.length() - 1);
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
  } else {
    onSelectBook(basepath + entry);
  }

  return;
}

void FileBrowserActivity::onDeleteEntry() {
  const std::string& entry = files[selectorIndex];
  bool isDirectory = (entry.back() == '/');

  std::string cleanBasePath = basepath;
  if (cleanBasePath.back() != '/') cleanBasePath += "/";
  const std::string fullPath = cleanBasePath + entry;

  auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
    if (!res.isCancelled) {
      LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
      if (!isDirectory) {
        clearFileMetadata(fullPath);
      }
      const bool deleted = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
      if (deleted) {
        LOG_DBG("FileBrowser", "Deleted successfully");
        loadFiles();
        if (files.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= files.size()) {
          // Move selection to the new "last" item
          selectorIndex = files.size() - 1;
        }

        requestUpdate(true);
      } else {
        LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
      }
    } else {
      LOG_DBG("FileBrowser", "Delete cancelled by user");
    }
  };

  std::string heading = tr(STR_DELETE) + std::string("? ");

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
}

void FileBrowserActivity::onRenameEntry() {
  const std::string& entry = files[selectorIndex];
  const bool isDirectory = (entry.back() == '/');
  const std::string currentName = isDirectory ? entry.substr(0, entry.length() - 1) : entry;

  std::string cleanBasePath = basepath;
  if (cleanBasePath.back() != '/') cleanBasePath += "/";

  auto handler = [this, cleanBasePath, currentName, isDirectory](const ActivityResult& res) {
    if (res.isCancelled) return;

    const std::string newName = std::get<KeyboardResult>(res.data).text;
    if (newName.empty() || newName == currentName) return;

    const std::string oldPath = cleanBasePath + currentName;
    const std::string newPath = cleanBasePath + newName;

    // Cache hash is path-derived, so the rename orphans the old entry; clear it.
    if (!isDirectory) clearFileMetadata(oldPath);

    if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
      LOG_DBG("FileBrowser", "Renamed %s -> %s", oldPath.c_str(), newPath.c_str());
      loadFiles();
      selectorIndex = findEntry(isDirectory ? newName + "/" : newName);
      requestUpdate(true);
    } else {
      LOG_ERR("FileBrowser", "Failed to rename: %s -> %s", oldPath.c_str(), newPath.c_str());
    }
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), currentName, 0, InputType::Text),
      handler);
}

void FileBrowserActivity::onGoToRoot() {
  basepath = "/";
  loadFiles();
  selectorIndex = 0;
  requestUpdate();
  return;
}

void FileBrowserActivity::onGoBack() {
  const std::string oldPath = basepath;

  basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
  if (basepath.empty()) basepath = "/";
  loadFiles();

  const auto pos = oldPath.find_last_of('/');
  const std::string dirName = oldPath.substr(pos + 1) + "/";
  selectorIndex = findEntry(dirName);

  requestUpdate();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}

std::vector<MenuRegistryEntry> FileBrowserActivity::getGlobalMenuEntries() {
  if (mode != Mode::Books) {
    return {};
  }

  std::vector<MenuRegistryEntry> entries = {};

  if (!files.empty()) {
    entries.insert(entries.end(), std::initializer_list<MenuRegistryEntry>{
      MenuRegistryEntry{
        .icon = Bitmap1Bit{ 40, 40, Filepencil40Icon },
        .name = tr(STR_RENAME),
        .onPress = [this]() { this->onRenameEntry(); }
      },
      MenuRegistryEntry{
        .icon = Bitmap1Bit{ 40, 40, Trashx40Icon },
        .name = tr(STR_DELETE),
        .onPress = [this]() { this->onDeleteEntry(); }
      }
    });
  }

  if(basepath != "/") {
    entries.insert(entries.end(), std::initializer_list<MenuRegistryEntry>{
      MenuRegistryEntry{
        .icon = Bitmap1Bit{ 40, 40, Arrowup40Icon },
        .name = tr(STR_GO_BACK),
        .onPress = [this]() {
          this->onGoBack();
        }
      },
      MenuRegistryEntry{
        .icon = Bitmap1Bit{ 40, 40, SlashIcon },
        .name = tr(STR_GO_TO_ROOT),
        .onPress = [this]() {
          this->onGoToRoot();
        }
      }
    });
  }

  return entries;
}
