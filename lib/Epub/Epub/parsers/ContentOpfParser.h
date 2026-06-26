#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_SUBJECT,     // dc:subject text (genres, newline-joined)
    IN_META_COLLECTION,  // EPUB3 <meta property="belongs-to-collection"> text (series)
    IN_META_GROUPPOS,    // EPUB3 <meta property="group-position"> text (series index)
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  HalFile tempItemStore;
  std::string coverItemId;

  // Index for fast idref→href lookup, built during manifest parsing
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  // True between dc:subject start and its first character chunk when `genre`
  // already holds an earlier subject — triggers a '\n' separator on first write.
  bool pendingSubjectSep = false;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string series;       // calibre:series or EPUB3 belongs-to-collection
  std::string seriesIndex;  // calibre:series_index or EPUB3 group-position (raw text)
  std::string genre;        // first dc:subject (primary genre)
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
