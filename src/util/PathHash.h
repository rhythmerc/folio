#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Canonical book-path -> cache key. MUST stay identical to the hash Epub uses
// to build its cache dir (Epub.cpp: "/epub_" + std::hash<path>) so the reader,
// LibraryIndex, and ProgressStore all agree on a single key per book.
inline uint32_t hashPath(const std::string& path) {
  return static_cast<uint32_t>(std::hash<std::string>{}(path));
}
