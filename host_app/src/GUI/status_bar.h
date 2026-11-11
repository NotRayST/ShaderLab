#pragma once
#include <cstdint>

namespace gui {

class GuiManager;
void render_bottom_status_bar(GuiManager &gui, float top_offset, float width, float height,
                              uint32_t img_w, uint32_t img_h,
                              float zoom_level, float pan_x, float pan_y);

} // namespace gui
