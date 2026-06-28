#include "ImageBlock.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstring>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath, bool ditherBw) {
  // Replace extension with the pixel-cache suffix. Fast mode stores a distinct
  // blue-noise BW cache (.pxcd) so it never collides with the Quality 4-level
  // cache (.pxc) — each mode keeps its own, neither rebuilds on toggle.
  const char* ext = ditherBw ? ".pxcd" : ".pxc";
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ext;
  }
  return imagePath + ext;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // BW-only RAM cache (GlobalMenu re-renders force BW). renderMode == BW also
  // excludes grayscale strip passes, so a partial strip can never poison it.
  const bool bwMode = renderer.getRenderMode() == GfxRenderer::BW;
  if (bwCache && bwMode) {
    blitBwCache(renderer, x, y);
    return;
  }

  // Fast mode reads/writes its own blue-noise BW cache, keyed separately from
  // the Quality 4-level cache so the two never collide.
  const bool ditherBw = renderer.isDitherBwActive();

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath, ditherBw);

  if (bwMode && renderer.bwImageCacheEnabled()) {
    if (buildAndBlitBwCache(renderer, cachePath, x, y)) {
      return;  // built + drew this frame; future re-renders blit from RAM
    }
    // build skipped/failed (no .pxc / OOM) — fall through to streaming
  }

  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherBlueNoise = ditherBw;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

void ImageBlock::blitBwCache(GfxRenderer& renderer, int x, int y) const {
  // One orientation-aware, byte-marching blit instead of a per-pixel walk.
  renderer.blitImage1Bit(bwCache.get(), (bwCacheWidth + 7) / 8, bwCacheWidth, bwCacheHeight, x, y);
}

bool ImageBlock::buildAndBlitBwCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }
  uint16_t cw, ch;
  if (cacheFile.read(&cw, 2) != 2 || cacheFile.read(&ch, 2) != 2) {
    return false;
  }
  if (abs(cw - width) > 1 || abs(ch - height) > 1) {
    LOG_ERR("IMG", "BW cache dim mismatch: %dx%d vs %dx%d", cw, ch, width, height);
    return false;
  }

  const int bytesPerRow2 = (cw + 3) / 4;  // source: 2 bits/pixel
  const int bytesPerRow1 = (cw + 7) / 8;  // dest:   1 bit/pixel
  const size_t total = static_cast<size_t>(bytesPerRow1) * ch;

  // ponytail: fixed 24KB reserve. If the 1bpp copy plus headroom for the menu's
  // own draw won't fit, skip the cache and stream as before — no cache, no crash.
  constexpr size_t RESERVE = 24 * 1024;
  if (ESP.getFreeHeap() < total + RESERVE) {
    LOG_DBG("IMG", "Skip BW cache: heap %u < need %u", (unsigned)ESP.getFreeHeap(), (unsigned)(total + RESERVE));
    return false;
  }
  auto buf = makeUniqueNoThrow<uint8_t[]>(total);
  if (!buf) {
    LOG_ERR("IMG", "OOM: BW cache %u bytes", (unsigned)total);
    return false;
  }
  memset(buf.get(), 0, total);

  // Stream the 2bpp .pxc through a small batched buffer (mirrors renderFromCache)
  // — never hold the whole 2bpp image — thresholding each pixel into buf.
  int rowsPerRead = 4096 / bytesPerRow2;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > ch) rowsPerRead = ch;
  auto readBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow2);
  if (!readBuffer) {
    rowsPerRead = 1;
    readBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow2);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "OOM: BW cache row buffer");
    return false;
  }

  // buf is zeroed (all bits 0 = ink/black per blit1Bit's convention); set a bit
  // only for white pixels (2-bit value == 3, matching the BW draw threshold).
  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < ch; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (ch - row < rowsPerRead) ? (ch - row) : rowsPerRead;
      const size_t bytes = static_cast<size_t>(toRead) * bytesPerRow2;
      if (cacheFile.read(readBuffer.get(), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "BW cache build read error at row %d", row);
        return false;  // buf discarded; caller streams/decodes and redraws fully
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* srcRow = readBuffer.get() + static_cast<size_t>(bufferRow) * bytesPerRow2;
    bufferRow++;
    uint8_t* dstRow = buf.get() + static_cast<size_t>(row) * bytesPerRow1;

    for (int col = 0; col < cw; col++) {
      const int byteIdx = col >> 2;
      const int bitShift = 6 - (col & 3) * 2;  // MSB-first within byte
      const uint8_t value = (srcRow[byteIdx] >> bitShift) & 0x03;
      if (value == 3) {  // white => background bit set; black (value < 3) stays 0
        dstRow[col >> 3] |= (0x80 >> (col & 7));
      }
    }
  }

  bwCache = std::move(buf);
  bwCacheWidth = cw;
  bwCacheHeight = ch;
  LOG_DBG("IMG", "Built BW cache: %dx%d (%u bytes)", cw, ch, (unsigned)total);

  // Draw this first frame from the freshly built cache (same fast blit path).
  blitBwCache(renderer, x, y);
  return true;
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
