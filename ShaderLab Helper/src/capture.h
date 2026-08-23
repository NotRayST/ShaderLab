#pragma once
#define ImTextureID ImU64
#include <reshade.hpp>

void on_reshade_finish_effects(
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view rtv_srgb
);
