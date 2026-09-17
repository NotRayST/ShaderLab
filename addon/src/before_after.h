#pragma once
#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>
#include "../../common/ipc_protocol.h"

struct BeforeAfterState {
    bool  enabled = false;
    float angle = 0.0f;        // degrees, 0 = vertical (left before, right after), 90 = horizontal (top before, bottom after)
    float split_offset = 0.0f; // offset from center [-0.5..0.5] along normal
    bool  is_dragging_pos = false;
    bool  is_dragging_rot = false;
};

BeforeAfterState &before_after_get_state();
void before_after_toggle();
bool before_after_handle_input(bool bg_hovered, bool any_active, bool fine);

void before_after_capture_pre(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv);

void before_after_composite(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv);

void on_before_after_destroy_device(reshade::api::device *device);
