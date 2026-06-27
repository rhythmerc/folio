#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

  // Release the RAM-resident BW pixel copy (if any). Called when the GlobalMenu
  // closes so the ~48KB is reclaimed.
  void releaseBwCache() {
    bwCache.reset();
    bwCacheWidth = bwCacheHeight = 0;
  }

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;

  // 1bpp RAM copy of the image (logical space, MSB-first; set bit = black),
  // built on the first BW render under the GlobalMenu so subsequent menu
  // re-renders blit from RAM instead of re-streaming the .pxc from SD.
  std::unique_ptr<uint8_t[]> bwCache;
  uint16_t bwCacheWidth = 0;
  uint16_t bwCacheHeight = 0;

  // Stream the .pxc once, threshold to the 1bpp bwCache, draw this frame.
  // Returns false (cache not built) on missing file / dim mismatch / OOM so the
  // caller falls back to the normal streaming path.
  bool buildAndBlitBwCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y);
  void blitBwCache(GfxRenderer& renderer, int x, int y) const;
};
