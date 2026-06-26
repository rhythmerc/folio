#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Shared zip handle reused across all helper calls. Lazy-initialised by zipRef().
  // Holding one instance avoids redundant EOCD scans and SD open/close cycles when
  // multiple items are read in sequence (e.g. during indexing).
  mutable std::unique_ptr<ZipFile> zip;

  ZipFile& zipRef() const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

 public:
  // Constructor and destructor defined in .cpp because zip is a unique_ptr to a
  // forward-declared ZipFile.
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub();
  std::string& getBasePath() { return contentBasePath; }
  // metadataOnly=true builds/accepts a lightweight cache (OPF-derived fields only:
  // title/author/series/genre/spineCount/cover). TOC + per-spine sizes are deferred;
  // the first full load (metadataOnly=false) completes the build. The library list
  // uses metadataOnly=true; the reader uses the default (full).
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false, bool metadataOnly = false);
  bool clearCache() const;
  bool setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  const std::string& getSeries() const;
  const std::string& getGenre() const;
  uint16_t getSeriesIndex() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int maxWidth, int maxHeight) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
