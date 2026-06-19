#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <i18n.h>
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "fontIds.h"
#include "components/icons/file40.h"
#include "components/icons/folder40.h"
#include "components/icons/arrowUp40.h"
#include "components/icons/slash.h"

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

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

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
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }

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
  GUI.drawHeader(renderer, Rect{0, td.layout.topPadding, pageWidth, td.header.height}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + td.layout.verticalSpacing;
  const int contentTop = td.layout.topPadding + td.header.height + td.layout.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - td.buttonHints.height - td.layout.verticalSpacing - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, td.layout.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - td.buttonHints.height - td.layout.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - td.layout.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - td.layout.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, td.layout.contentSidePadding, pathY, pathDisplay);
  }

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

  ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

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

  const std::string& entry = files[selectorIndex];
  bool isDirectory = (entry.back() == '/');

  std::vector<MenuRegistryEntry> entries = {};

  if (!files.empty()) {
    entries.push_back(MenuRegistryEntry{
      .icon = Bitmap1Bit{
        40,
        40,
        isDirectory ? Folder40Icon : File40Icon
      },
      .name = isDirectory ? tr(STR_SELECTED_DIRECTORY) : tr(STR_SELECTED_FILE),
      .popupItems = {
        PopupMenuEntry{
          .label = tr(STR_OPEN),
          .onSelected = [this]() { 
            this->onSelectEntry();
            return true;
          }
        },
        PopupMenuEntry{
          .label = tr(STR_DELETE),
          .onSelected = [this]() {
            this->onDeleteEntry();
            return true; 
          }
        }
      }
    });
  };

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
