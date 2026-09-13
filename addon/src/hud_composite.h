#pragma once
#include <reshade.hpp>
#include "../../common/ipc_protocol.h"

// composites the host's HUD layers onto the backbuffer after reshade effects run,
// otherwise post-processing shaders end up warping the app UI
void composite_hud(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv);

// release the cached D3D11 pipeline objects before the device goes away
void on_hud_destroy_device(reshade::api::device *device);
