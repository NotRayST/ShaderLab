#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "overlay_ui.h"
#include "capture.h"
#include "job_queue.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) {
            return FALSE;
        }
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(&on_reshade_finish_effects);
        reshade::register_overlay("ShaderLab Helper", &on_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay("ShaderLab Helper", &on_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&on_reshade_finish_effects);
        JobQueueManager::get().shutdown();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
