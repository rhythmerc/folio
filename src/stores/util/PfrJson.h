#pragma once

#include <ArduinoJson.h>
#include <boost/pfr.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Generic struct<->JSON mapping driven by Boost.PFR reflection over ArduinoJson.
//
// A plain aggregate struct *is* the schema: field names and types are read at
// compile time, so a store never hand-writes a per-field mapping. write()/read()
// recurse through arithmetic/enum/bool, std::string, std::vector, and nested
// aggregates. Each distinct type instantiates one write<T>/read<T>; the set of
// types is bounded by the store's data model, so template growth is bounded.
//
// Supported field types: bool, integral/floating, enum, std::string,
// std::vector<Supported>, and aggregate structs of Supported fields. Anything
// else is a compile error (the static_assert below), by design.
namespace pfrjson {

// Type trait: is T a std::vector specialization?
template <class>
struct is_std_vector : std::false_type {};
template <class U, class A>
struct is_std_vector<std::vector<U, A>> : std::true_type {};

template <class T>
void write(JsonVariant v, const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    v.set(value);
  } else if constexpr (std::is_enum_v<T>) {
    v.set(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    v.set(value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    v.set(value);  // ArduinoJson copies the string into the document
  } else if constexpr (is_std_vector<T>::value) {
    JsonArray arr = v.to<JsonArray>();
    for (const auto& elem : value) {
      write(arr.add<JsonVariant>(), elem);
    }
  } else {
    static_assert(std::is_aggregate_v<T>,
                  "pfrjson::write: unsupported type (expected arithmetic/enum/string/vector/aggregate)");
    JsonObject obj = v.to<JsonObject>();
    boost::pfr::for_each_field_with_name(value, [&](std::string_view name, const auto& field) {
      // std::string key so ArduinoJson copies it (PFR names_as_array views are
      // not null-terminated at the field boundary, so .data() is not a C string).
      write(obj[std::string(name)].template to<JsonVariant>(), field);
    });
  }
}

template <class T>
void read(JsonVariantConst v, T& out) {
  if constexpr (std::is_same_v<T, bool>) {
    out = v.as<bool>();
  } else if constexpr (std::is_enum_v<T>) {
    out = static_cast<T>(v.as<std::underlying_type_t<T>>());
  } else if constexpr (std::is_arithmetic_v<T>) {
    out = v.as<T>();  // missing/!convertible -> 0
  } else if constexpr (std::is_same_v<T, std::string>) {
    out = v.as<std::string>();  // missing/!string -> ""
  } else if constexpr (is_std_vector<T>::value) {
    out.clear();
    JsonArrayConst arr = v.as<JsonArrayConst>();
    out.reserve(arr.size());
    for (JsonVariantConst elem : arr) {
      typename T::value_type tmp{};
      read(elem, tmp);
      out.push_back(std::move(tmp));
    }
  } else {
    static_assert(std::is_aggregate_v<T>,
                  "pfrjson::read: unsupported type (expected arithmetic/enum/string/vector/aggregate)");
    JsonObjectConst obj = v.as<JsonObjectConst>();
    boost::pfr::for_each_field_with_name(out, [&](std::string_view name, auto& field) {
      read(obj[std::string(name)], field);
    });
  }
}

}  // namespace pfrjson
