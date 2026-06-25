#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "LibraryIndex.h"  // LibraryBook

// Resolves "which subset of the library does the user want?" into an ordered book list.
//
// A library view is a named subset, and the only difference between kinds is HOW
// membership is computed: a manual collection joins against CollectionStore membership;
// series/author/genre match a metadata field; search runs the index query; ALL is the
// whole index. This service owns that selection so the view model doesn't have to.
//
// It sits ABOVE LibraryIndex and CollectionStore (composes both), keeping the low-level
// book index free of any collection dependency. Stateless — pure function of the
// current index + store contents, so the API is static.
class LibrarySubsetManager {
 public:
  // Spec alternatives are nested to scope their (generic) names and, in particular, to
  // avoid clashing with the existing top-level `Collection` (CollectionStore.h): hence
  // CollectionRef ("a collection, by id") vs. the stored Collection.
  struct All {};
  struct CollectionRef {
    uint32_t id;
  };
  struct MetadataGroup {
    enum Field : uint8_t { Series, Author, Genre } field;
    std::string_view name;
  };
  struct Search {
    std::string_view query;
  };
  using Spec = std::variant<All, CollectionRef, MetadataGroup, Search>;

  struct Resolved {
    // Pointers into LIBRARY_INDEX.getBooks(), in index (sorted) order — same validity
    // contract as getBooks()/search() (valid until the next sortBy()/load).
    std::vector<const LibraryBook*> books;
    // The subset's own display name (collection name, group/query string); empty for
    // All. The caller decides how to present an empty title (e.g. "Library").
    std::string title;
  };

  // Resolve a spec to its books + title. Returns std::nullopt ONLY when the spec names a
  // collection id that no longer exists (stale) — the caller treats that as "fall back".
  // Every other spec yields a value; an empty `books` is a valid result (an empty
  // collection, a search with no hits, a group that matches nothing).
  static std::optional<Resolved> resolve(const Spec& spec);
};
