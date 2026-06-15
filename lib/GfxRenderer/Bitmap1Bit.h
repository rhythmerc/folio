#pragma once

#include <cstdint>

// In-memory 1-bit packed raster: MSB-first, each row padded to a whole byte
// (stride = (width + 7) / 8). A `0` bit is ink, a `1` bit is background and is
// left transparent by the renderer's icon blit. Use for in-flash icon arrays
// (e.g. src/components/icons/*.h) drawn via GfxRenderer::drawIcon.
//
// Field order matches the designated initializers used at call sites
// (`{.width = .., .height = .., .bitmap = ..}`); keep it stable.
struct Bitmap1Bit {
  int width;
  int height;
  const uint8_t* bitmap;
};
