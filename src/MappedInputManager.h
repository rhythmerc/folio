#pragma once

#include <HalGPIO.h>
#include <GfxRenderer.h>


class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
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

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

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

  // See HalGPIO::suppressHeldConsumedReleases — call on a control hand-off so a
  // press handled here doesn't leak its release to the next screen/menu.
  void suppressHeldConsumedReleases() const { gpio.suppressHeldConsumedReleases(); }

 private:
  HalGPIO& gpio;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  static std::vector<Button> allButtonsForDirection(DirectionalButton direction, GfxRenderer::Orientation orientation);
};
