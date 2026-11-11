#pragma once
#include <cstdint>

namespace gui {

class GuiManager;
void render_center_canvas(GuiManager &gui, float left_offset, float top_offset, float canvas_width, float canvas_height,
                          uint32_t img_w, uint32_t img_h,
                          float zoom_level, float pan_x, float pan_y);

} // namespace gui
