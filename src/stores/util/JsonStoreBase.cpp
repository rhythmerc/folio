#include "JsonStoreBase.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint";

// ArduinoJson reads from a "reader" exposing read()/readBytes(). HalFile is a
// Print (write side) but not an Arduino Stream, so this thin adapter lets us
// stream-parse straight from the file instead of buffering the whole thing in
// a String first.
struct HalFileReader {
  FsFile& f;
  int read() { return f.read(); }
  size_t readBytes(char* buffer, size_t length) {
    const int n = f.read(buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
};
}  // namespace

bool JsonStoreBase::writeDoc(const JsonDocument& doc) const {
  Storage.mkdir(CACHE_DIR);

  const std::string tmpPath = std::string(path_) + ".tmp";

  {
    FsFile f;
    if (!Storage.openFileForWrite(tag_, tmpPath.c_str(), f)) {
      LOG_ERR(tag_, "Cannot open %s for write", tmpPath.c_str());
      return false;
    }
    if (serializeJson(doc, f) == 0) {
      LOG_ERR(tag_, "Failed to write JSON to %s", tmpPath.c_str());
      return false;
    }
    // Must close before rename — see CLAUDE.md DESTRUCTOR_CLOSES_FILE note.
    f.close();
  }

  if (Storage.exists(path_)) {
    Storage.remove(path_);
  }
  if (!Storage.rename(tmpPath.c_str(), path_)) {
    LOG_ERR(tag_, "Failed to rename %s -> %s", tmpPath.c_str(), path_);
    return false;
  }
  return true;
}

bool JsonStoreBase::readDoc(JsonDocument& doc) const {
  if (!Storage.exists(path_)) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(tag_, path_, f)) {
    LOG_ERR(tag_, "Cannot open %s for read", path_);
    return false;
  }

  HalFileReader reader{f};
  const DeserializationError err = deserializeJson(doc, reader);
  if (err) {
    LOG_ERR(tag_, "JSON parse error in %s: %s", path_, err.c_str());
    return false;
  }
  return true;
}
