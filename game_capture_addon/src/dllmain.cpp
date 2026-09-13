#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "capture_manager.h"

extern "C" __declspec(dllexport) const char* NAME = "ShaderLab Capture";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Captures in-game depth for ShaderLab.";

static void on_begin_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    CaptureManager::get().on_reshade_begin_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_finish_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    CaptureManager::get().on_reshade_finish_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_screenshot(reshade::api::effect_runtime* runtime, const char* path) {
    CaptureManager::get().on_reshade_screenshot(runtime, path);
}

static void on_reloaded_effects(reshade::api::effect_runtime* runtime) {
    CaptureManager::get().on_reshade_reloaded_effects(runtime);
}

static void on_overlay(reshade::api::effect_runtime* runtime) {
    CaptureManager::get().on_draw_overlay(runtime);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) {
            return FALSE;
        }
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(&on_begin_effects);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
        reshade::register_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::register_overlay(nullptr, &on_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(nullptr, &on_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&on_begin_effects);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
