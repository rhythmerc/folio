#include <optional>
#include "components/themes/BaseTheme.h"
#include "MappedInputManager.h"
#include "util/Flex.h"

class UIPage {
  public:
    static Rect render(
        GfxRenderer& renderer,
        const char* title,
        const char* subtitle,
        const MappedInputManager::Labels btnLabels,
        const std::optional<flex::Padding> paddingOverride = std::nullopt
    );
};
