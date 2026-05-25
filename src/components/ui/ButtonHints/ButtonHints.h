#pragma once

class GfxRenderer;

// Front and side button hint rendering.
//
// Stateless — callers pass the labels and the active theme is consulted via
// the GUI singleton at render time. Pass nullptr or "" for any slot to hide
// that hint (the visual style decides whether the slot's frame is still drawn).
class ButtonHints {
 public:
  // Front buttons (bottom edge in portrait). The renderer's orientation is
  // temporarily forced to Portrait so hints stay aligned to the physical
  // buttons regardless of the active reading orientation.
  static void render(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4);

  // Side buttons (right edge in portrait, X3 layout is left+right).
  static void renderSide(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn);
};
