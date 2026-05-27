#pragma once

#include <ReaderFontRegistry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FontSelectionActivity final : public Activity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const ReaderFontRegistry* registry);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // index used by valueSetter
  };

  const ReaderFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
};
