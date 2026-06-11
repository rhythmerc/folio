#pragma once

#include <ArduinoJson.h>

#include "JsonStoreBase.h"
#include "PfrJson.h"

// JSON-backed store generic over its payload type. T is a plain aggregate whose
// fields ARE the on-disk schema (serialized via Boost.PFR reflection), so a
// concrete store writes no serialization code at all — it just declares T and
// its domain methods over data_.
//
// Minimal store:
//   struct FooData { int a; std::string b; };
//   class FooStore : public JsonStore<FooData> {
//     FooStore() : JsonStore("/.crosspoint/foo.json", "FOO") {}
//     // ... domain methods over data_ ...
//   };
//
// Override the optional hooks only when needed:
//   - onLoaded():      recompute non-persisted/derived state after a load.
//   - migrateLegacy(): import an older on-disk format when the JSON is absent.
template <class T>
class JsonStore : public JsonStoreBase {
  static_assert(std::is_aggregate_v<T>, "JsonStore<T>: T must be an aggregate struct");

 public:
  bool saveToFile() const {
    JsonDocument doc;
    pfrjson::write(doc.template to<JsonVariant>(), data_);
    return writeDoc(doc);
  }

  bool loadFromFile() {
    data_ = T{};       // an absent/invalid file is a valid (empty) state
    loaded_ = true;
    JsonDocument doc;
    if (readDoc(doc)) {
      pfrjson::read(doc.template as<JsonVariantConst>(), data_);
      onLoaded();
      return true;
    }
    onLoaded();
    return migrateLegacy();
  }

 protected:
  JsonStore(const char* path, const char* tag) : JsonStoreBase(path, tag) {}
  ~JsonStore() = default;

  T data_{};

  virtual void onLoaded() {}
  virtual bool migrateLegacy() { return false; }
};
