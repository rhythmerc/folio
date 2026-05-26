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
  //
  // `powerBtn` is an optional hint for the device's power button. When
  // non-empty, an extra slot is painted above the top side button on the X4
  // layout — anchored so the power-hint center sits 15 mm above the top
  // side-button center (matches the Xteink X4 hardware spacing). X3 hardware
  // doesn't have a power button alongside the side rail, so the power slot
  // is silently dropped on X3.
  static void renderSide(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                         const char* powerBtn = nullptr);
};
