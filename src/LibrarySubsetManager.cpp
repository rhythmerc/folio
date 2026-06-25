#include "LibrarySubsetManager.h"

#include <unordered_set>

#include "stores/collections/CollectionStore.h"

std::optional<LibrarySubsetManager::Resolved> LibrarySubsetManager::resolve(
  const Spec& spec
) {
  const auto& books = LIBRARY_INDEX.getBooks();
  Resolved out;

  if (std::get_if<All>(&spec) != nullptr) {
    out.books.reserve(books.size());
    for (const auto& b : books) out.books.push_back(&b);
    return out;  // title stays empty — caller substitutes "Library"
  }

  if (const auto* c = std::get_if<CollectionRef>(&spec)) {
    if (!COLLECTION_STORE.isLoaded()) COLLECTION_STORE.loadFromFile();
    const Collection* coll = COLLECTION_STORE.findById(c->id);
    if (coll == nullptr) return std::nullopt;  // stale id — caller falls back to ALL

    const std::unordered_set<uint32_t> members(
      coll->members.begin(), coll->members.end()
    );
    for (const auto& b : books) {
      if (members.count(b.pathHash)) out.books.push_back(&b);
    }
    out.title = coll->name;  // empty membership is allowed (back-tile-only view)
    return out;
  }

  if (const auto* g = std::get_if<MetadataGroup>(&spec)) {
    for (const auto& b : books) {
      // Genre holds multiple newline-joined subjects: a book belongs if any matches.
      if (g->field == MetadataGroup::Genre) {
        bool match = false;
        forEachGenre(b.genre, [&](std::string_view subject) {
          if (subject == g->name) match = true;
        });
        if (match) out.books.push_back(&b);
        continue;
      }
      const std::string& field =
        (g->field == MetadataGroup::Series) ? b.series : b.author;
      if (field == g->name) out.books.push_back(&b);
    }
    out.title = std::string(g->name);
    return out;
  }

  const auto& s = std::get<Search>(spec);
  out.books = LIBRARY_INDEX.search(s.query);
  out.title = std::string(s.query);
  return out;
}
