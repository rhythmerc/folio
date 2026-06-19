#include "MappedInputManager.h"

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

std::vector<MappedInputManager::Button> MappedInputManager::allButtonsForDirection(MappedInputManager::DirectionalButton btn, GfxRenderer::Orientation orientation) {
  const auto btns = directionalButtonsForOrientation(orientation);
  switch(btn) {
    case DirectionalButton::Up:
      return btns.Up;
    case DirectionalButton::Down:
      return btns.Down;
    case DirectionalButton::Left:
      return btns.Left;
    case DirectionalButton::Right:
      return btns.Right;
    default:
      return std::vector<MappedInputManager::Button>{};
  }
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }
bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }
bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasPressed(const DirectionalButton button, GfxRenderer::Orientation orientation) const { 
  auto allBtns = allButtonsForDirection(button, orientation);
  for (auto btn : allBtns) {
    if (wasPressed(btn)) {
      return true;
    }
  }

  return false;
}

bool MappedInputManager::wasReleased(const DirectionalButton button, GfxRenderer::Orientation orientation) const { 
  auto allBtns = allButtonsForDirection(button, orientation);
  for (auto btn : allBtns) {
    if (wasReleased(btn)) {
      return true;
    }
  }

  return false;
}

bool MappedInputManager::isPressed(const DirectionalButton button, GfxRenderer::Orientation orientation) const { 
  auto allBtns = allButtonsForDirection(button, orientation);
  for (auto btn : allBtns) {
    if (isPressed(btn)) {
      return true;
    }
  }

  return false;
}

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return rightLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

MappedInputManager::DirectionalButtons MappedInputManager::directionalButtonsForOrientation(
    GfxRenderer::Orientation orientation
) {
  switch(orientation) {
    case GfxRenderer::Orientation::Portrait:
      return DirectionalButtons{
        .Up = { MappedInputManager::Button::Up },
        .Down = { MappedInputManager::Button::Down },
        .Left = { MappedInputManager::Button::Left, MappedInputManager::Button::Back },
        .Right = { MappedInputManager::Button::Right, MappedInputManager::Button::Confirm },
      };
    case GfxRenderer::Orientation::PortraitInverted:
      return DirectionalButtons{
        .Up = { MappedInputManager::Button::Down },
        .Down = { MappedInputManager::Button::Up },
        .Left = { MappedInputManager::Button::Right, MappedInputManager::Button::Confirm },
        .Right = { MappedInputManager::Button::Left, MappedInputManager::Button::Back },
      };
    case GfxRenderer::Orientation::LandscapeClockwise:
      return DirectionalButtons{
        .Up = { MappedInputManager::Button::Left, MappedInputManager::Button::Back },
        .Down = { MappedInputManager::Button::Right, MappedInputManager::Button::Confirm },
        .Left = { MappedInputManager::Button::Down },
        .Right = { MappedInputManager::Button::Up },
      };
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      return DirectionalButtons{
        .Up = { MappedInputManager::Button::Right, MappedInputManager::Button::Confirm },
        .Down = { MappedInputManager::Button::Left, MappedInputManager::Button::Back },
        .Left = { MappedInputManager::Button::Up },
        .Right = { MappedInputManager::Button::Down }
      };
    default:
      return DirectionalButtons{};
  }
}
