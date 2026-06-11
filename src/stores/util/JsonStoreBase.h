#pragma once

#include <ArduinoJson.h>

// Type-independent file mechanics for JSON-backed stores: directory creation,
// atomic temp-write + rename, existence/parse handling, the loaded flag and
// error logging. The typed layer lives in JsonStore<T> (JsonStore.h); this base
// keeps HalStorage and the file I/O out of headers that derived stores include.
class JsonStoreBase {
 public:
  bool isLoaded() const { return loaded_; }

 protected:
  // path: backing file, e.g. "/.crosspoint/collections.json".
  // tag: short module tag for logging, e.g. "COL".
  JsonStoreBase(const char* path, const char* tag) : path_(path), tag_(tag) {}
  ~JsonStoreBase() = default;

  // Serialize doc to the backing file via a temp file + rename, so a crash
  // mid-write cannot corrupt the existing file. Returns false on I/O failure.
  bool writeDoc(const JsonDocument& doc) const;

  // Parse the backing file into doc. Returns false if the file is absent or
  // malformed (the caller treats both as "no data to load").
  bool readDoc(JsonDocument& doc) const;

  const char* path_;
  const char* tag_;
  bool loaded_ = false;
};
