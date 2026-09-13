#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "overlay_ui.h"
#include "capture.h"
#include "job_queue.h"
#include "hud_composite.h"
#include "undo_history.h"

extern "C" __declspec(dllexport) const char *NAME = "ShaderLab";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "ShaderLab Helper Addon v1.2.1 by NotRaySt";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) {
            return FALSE;
        }
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(&on_reshade_begin_effects);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reshade_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(&on_reshade_finish_effects);
        reshade::register_event<reshade::addon_event::destroy_device>(&on_hud_destroy_device);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_overlay("ShaderLab Helper", &on_overlay);
        undo_history::init();
        break;
    case DLL_PROCESS_DETACH:
        undo_history::shutdown();
        reshade::unregister_overlay("ShaderLab Helper", &on_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_event<reshade::addon_event::destroy_device>(&on_hud_destroy_device);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&on_reshade_finish_effects);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&on_reshade_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&on_reshade_begin_effects);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        JobQueueManager::get().shutdown();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
