#pragma once

#include <Bitmap1Bit.h>
#include <string>

struct MenuRegistryEntry {
  using Icon = Bitmap1Bit;
  Icon icon;
  std::string name;
};
