#include "before_after.h"

namespace {
BeforeAfterState s_state;
}

BeforeAfterState &before_after_get_state() {
    return s_state;
}

void before_after_toggle() {
    s_state.enabled = !s_state.enabled;
    if (!s_state.enabled) {
        s_state.is_dragging_pos = false;
        s_state.is_dragging_rot = false;
    }
}

void before_after_capture_pre(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv)
{
    (void)block; (void)runtime; (void)cmd_list; (void)rtv;
}

void before_after_composite(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv)
{
    (void)block; (void)runtime; (void)cmd_list; (void)rtv;
}

void on_before_after_destroy_device(reshade::api::device *device) {
    (void)device;
}
