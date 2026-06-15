#include "UIPage.h"
#include "components/UITheme.h"
#include "GfxRenderer.h"
#include "util/Flex.h"
#include "components/ui/ButtonHints/ButtonHints.h"

Rect UIPage::render(
    GfxRenderer& renderer,
    const char* title,
    const char* subtitle,
    MappedInputManager::Labels btnLabels,
    std::optional<flex::Padding> paddingOverride
) {
  const auto& td = *GUI.getData();
  const Rect screen{ 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight() };

  flex::Vstack page(
    screen,
    { 
      flex::fixed(td.layout.topPadding),
      flex::fixed(td.header.height),
      flex::grow(),
      flex::fixed(td.buttonHints.height)
    }
  );

  const auto top = flex::join(page[0], page[1]);
  renderer.fillRect(top.x, top.y, top.width, top.height, false);

  GUI.drawHeader(renderer, page[1], title, subtitle);

  const auto body = page[2];
  const auto bodyWithPadding = flex::inset(
      body,
      paddingOverride.value_or(flex::Padding{ 
          .top = static_cast<int16_t>(td.layout.topPadding),
          .right = static_cast<int16_t>(td.layout.contentSidePadding),
          .bottom = 0,
          .left = static_cast<int16_t>(td.layout.contentSidePadding)
        }
      )
  );


  ButtonHints::render(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);

  return bodyWithPadding;
}
