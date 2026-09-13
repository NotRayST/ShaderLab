#pragma once
#define ImTextureID ImU64
#include <reshade.hpp>

void on_reshade_begin_effects(
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view rtv_srgb
);

void on_reshade_finish_effects(
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view rtv_srgb
);

void on_reshade_reloaded_effects(
    reshade::api::effect_runtime *runtime
);

void on_init_effect_runtime(
    reshade::api::effect_runtime *runtime
);

void on_destroy_effect_runtime(
    reshade::api::effect_runtime *runtime
);

void on_reshade_present(
    reshade::api::effect_runtime *runtime
);

// true if reshade is currently compiling or loading effects on worker threads
bool is_effects_loading();
