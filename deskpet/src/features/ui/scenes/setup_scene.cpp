#include "src/features/ui/scenes/scenes.h"

#include "src/features/ui/ui_draw.h"

namespace ui {
void drawConfig(const AppState &state) {
  header("MAKE YOURSELF AT HOME", state.battery);
  uiText(18, 39, "01  Connect your phone", FONT_BODY, COLOR_TEXT);
  uiText(18, 60, state.setup.apName, FONT_TITLE, COLOR_TEXT);
  uiText(18, 89, "PASSWORD", FONT_SMALL, COLOR_TEXT);
  uiText(18, 105, state.setup.apPassword, FONT_TITLE, COLOR_TEXT);
  uiText(18, 137, "02  Open 192.168.4.1", FONT_BODY, COLOR_TEXT);
  pill(SETUP_DONE_BUTTON, "Done, back to my companion", COLOR_ACCENT);
  uiCentered(220, "Wi-Fi, location, name and brightness", FONT_SMALL, COLOR_TEXT);
}
}  // namespace ui
