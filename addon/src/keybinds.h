#pragma once

#include <imgui.h>
#include "../../common/ipc_protocol.h"

namespace reshade { namespace api { struct effect_runtime; } }

// configurable hotkeys, shared between the undo/redo engine and viewport input
//
// keybinds live in ReShade's config file under "ShaderLab" -> "Keybinds" so they
// survive restarts. each action gets a main key plus optional ctrl/shift/alt
// modifiers. kept this self-contained (only needs the reshade addon api + imgui)
// so it can get lifted out into its own addon alongside the undo engine later
//
// call init() once a runtime is available (init_effect_runtime works) to load the
// saved bindings, call shutdown() on unload. writes get persisted lazily through
// the stored runtime pointer
namespace keybinds {

using Action = IpcAction;

void init(reshade::api::effect_runtime *runtime);
void shutdown();

bool is_pressed(Action action); // key + mods were pressed this frame
bool is_down(Action action);    // key + mods are held (for FineTune)
ImGuiKey key(Action action);
bool has_ctrl(Action action);
bool has_shift(Action action);
bool has_alt(Action action);

void set_key(Action action, ImGuiKey key, bool ctrl, bool shift, bool alt);
void reset_to_defaults();
const char *name(Action action);     // human label, e.g. "Undo"
void describe(Action action, char *buf, size_t size); // e.g. "Ctrl+Z" / "Shift"
uint32_t to_vk(Action action);
uint32_t fine_tune_vk();             // VK code for the FineTune binding (for host-side detection)
uint32_t imgui_key_to_vk(ImGuiKey k);

bool is_modifier_key(ImGuiKey key);
bool is_ctrl_key(ImGuiKey key);
bool is_shift_key(ImGuiKey key);
bool is_alt_key(ImGuiKey key);

void sync_to_ipc(SharedControlBlock *block = nullptr);

} // namespace keybinds
