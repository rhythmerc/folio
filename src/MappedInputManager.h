#pragma once

#include <HalGPIO.h>
#include <GfxRenderer.h>


class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  enum class DirectionalButton { Up, Down, Left, Right };
  struct DirectionalButtons {
    std::vector<Button> Up;
    std::vector<Button> Down;
    std::vector<Button> Left;
    std::vector<Button> Right;
  };

  static DirectionalButtons directionalButtonsForOrientation(GfxRenderer::Orientation orientation);

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const { gpio.update(); }

  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;

  // DirectionalButton overloads map to the logical set of physical buttons for the given orientation.
  // For example, in Landscape CW, Up corresponds to what portrait calls Back and Left,
  // while Left corresponds to what portrait calls Down.
  bool wasPressed(DirectionalButton button, GfxRenderer::Orientation orientation) const;
  bool wasReleased(DirectionalButton button, GfxRenderer::Orientation orientation) const;
  bool isPressed(DirectionalButton button, GfxRenderer::Orientation orientation) const;

  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;
  // See HalGPIO::suppressHeldConsumedReleases — call on a control hand-off so a
  // press handled here doesn't leak its release to the next screen/menu.
  void suppressHeldConsumedReleases() const { gpio.suppressHeldConsumedReleases(); }

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  static std::vector<Button> allButtonsForDirection(DirectionalButton direction, GfxRenderer::Orientation orientation);
};
