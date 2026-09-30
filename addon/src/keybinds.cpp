#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "keybinds.h"
#include "job_queue.h"

#include <windows.h>
#include <cstdio>

namespace {

constexpr const char *kSection = "ShaderLab";
constexpr const char *kKeyPrefix = "Keybind";

struct Binding {
    ImGuiKey key;
    bool ctrl;
    bool shift;
    bool alt;
};

Binding defaults[] = {
    { ImGuiKey_Z,             true,  false, false }, // Undo
    { ImGuiKey_Y,             true,  false, false }, // Redo
    { ImGuiKey_R,             false, false, false }, // ResetRotation
    { ImGuiKey_Q,             false, false, false }, // ResetZoomPan
    { ImGuiKey_LeftArrow,     false, false, false }, // NudgeLeft
    { ImGuiKey_RightArrow,    false, false, false }, // NudgeRight
    { ImGuiKey_LeftShift,     false, false, false }, // FineTune (held)
    { ImGuiKey_H,             false, false, false }, // LockPan
    { ImGuiKey_J,             false, false, false }, // LockZoom
    { ImGuiKey_K,             false, false, false }, // LockRotate
    { ImGuiKey_L,             false, false, false }, // LockView
    { ImGuiKey_F11,           false, false, false }, // ToggleFullscreen
    { ImGuiKey_S,             true,  false, false }, // SaveProject (Ctrl+S)
    { ImGuiKey_S,             true,  true,  false }, // SaveProjectAs (Ctrl+Shift+S)
    { ImGuiKey_E,             true,  false, false }, // ExportImage (Ctrl+E)
    { ImGuiKey_E,             true,  true,  false }, // ExportImageAs (Ctrl+Shift+E)
    { ImGuiKey_B,             false, false, false }, // ToggleBeforeAfter (B)
    { ImGuiKey_E,             false, false, false }, // ToggleErase (E)
};
static_assert(sizeof(defaults) / sizeof(defaults[0]) == static_cast<size_t>(keybinds::Action::Count), "defaults array size must match Action::Count");

Binding current[static_cast<int>(keybinds::Action::Count)];
reshade::api::effect_runtime *g_runtime = nullptr;

Binding &binding(keybinds::Action a)
{
    return current[static_cast<int>(a)];
}

void load_binding(keybinds::Action a)
{
    Binding &b = binding(a);
    b = defaults[static_cast<int>(a)];

    if (g_runtime == nullptr)
        return;

    char buf[64];
    const char *section = kSection;
    const char *base = kKeyPrefix;

    snprintf(buf, sizeof(buf), "%s%02dKey", base, static_cast<int>(a));
    int key = static_cast<int>(b.key);
    reshade::get_config_value(g_runtime, section, buf, key);
    b.key = static_cast<ImGuiKey>(key);

    snprintf(buf, sizeof(buf), "%s%02dCtrl", base, static_cast<int>(a));
    reshade::get_config_value(g_runtime, section, buf, b.ctrl);
    snprintf(buf, sizeof(buf), "%s%02dShift", base, static_cast<int>(a));
    reshade::get_config_value(g_runtime, section, buf, b.shift);
    snprintf(buf, sizeof(buf), "%s%02dAlt", base, static_cast<int>(a));
    reshade::get_config_value(g_runtime, section, buf, b.alt);
}

void save_binding(keybinds::Action a)
{
    if (g_runtime == nullptr)
        return;

    const Binding &b = binding(a);
    const char *section = kSection;
    const char *base = kKeyPrefix;
    char buf[64];

    snprintf(buf, sizeof(buf), "%s%02dKey", base, static_cast<int>(a));
    reshade::set_config_value(g_runtime, section, buf, static_cast<int>(b.key));
    snprintf(buf, sizeof(buf), "%s%02dCtrl", base, static_cast<int>(a));
    reshade::set_config_value(g_runtime, section, buf, b.ctrl);
    snprintf(buf, sizeof(buf), "%s%02dShift", base, static_cast<int>(a));
    reshade::set_config_value(g_runtime, section, buf, b.shift);
    snprintf(buf, sizeof(buf), "%s%02dAlt", base, static_cast<int>(a));
    reshade::set_config_value(g_runtime, section, buf, b.alt);
}

} // namespace

namespace keybinds {

bool is_modifier_key(ImGuiKey key)
{
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
           key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
           key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper;
}

bool is_ctrl_key(ImGuiKey key)  { return key == ImGuiKey_LeftCtrl  || key == ImGuiKey_RightCtrl; }
bool is_shift_key(ImGuiKey key) { return key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift; }
bool is_alt_key(ImGuiKey key)   { return key == ImGuiKey_LeftAlt   || key == ImGuiKey_RightAlt; }

void init(reshade::api::effect_runtime *runtime)
{
    g_runtime = runtime;
    for (int i = 0; i < static_cast<int>(Action::Count); ++i)
        load_binding(static_cast<Action>(i));
    sync_to_ipc();
}

void shutdown()
{
    g_runtime = nullptr;
}

bool is_pressed(Action action)
{
    const Binding &b = binding(action);
    if (!ImGui::IsKeyPressed(b.key, false))
        return false;

    ImGuiIO &io = ImGui::GetIO();
    if (b.ctrl != io.KeyCtrl && !is_ctrl_key(b.key)) return false;
    if (b.shift != io.KeyShift && !is_shift_key(b.key)) return false;
    if (b.alt != io.KeyAlt && !is_alt_key(b.key)) return false;
    return true;
}

bool is_down(Action action)
{
    const Binding &b = binding(action);
    if (!ImGui::IsKeyDown(b.key))
        return false;

    ImGuiIO &io = ImGui::GetIO();
    if (b.ctrl != io.KeyCtrl && !is_ctrl_key(b.key)) return false;
    if (b.shift != io.KeyShift && !is_shift_key(b.key)) return false;
    if (b.alt != io.KeyAlt && !is_alt_key(b.key)) return false;
    return true;
}

ImGuiKey key(Action action) { return binding(action).key; }
bool has_ctrl(Action action) { return binding(action).ctrl; }
bool has_shift(Action action) { return binding(action).shift; }
bool has_alt(Action action) { return binding(action).alt; }

void set_key(Action action, ImGuiKey key, bool ctrl, bool shift, bool alt)
{
    Binding &b = binding(action);
    b.key = key;
    b.ctrl = ctrl;
    b.shift = shift;
    b.alt = alt;
    save_binding(action);
    sync_to_ipc();
}

void reset_to_defaults()
{
    for (int i = 0; i < static_cast<int>(Action::Count); ++i)
        binding(static_cast<Action>(i)) = defaults[i];
    for (int i = 0; i < static_cast<int>(Action::Count); ++i)
        save_binding(static_cast<Action>(i));
    sync_to_ipc();
}

const char *name(Action action)
{
    switch (action) {
    case Action::Undo: return "Undo";
    case Action::Redo: return "Redo";
    case Action::ResetRotation: return "Reset rotation";
    case Action::ResetZoomPan: return "Reset zoom / pan";
    case Action::NudgeLeft: return "Nudge Left";
    case Action::NudgeRight: return "Nudge Right";
    case Action::FineTune: return "Fine-tune (held)";
    case Action::LockPan: return "Lock Pan";
    case Action::LockZoom: return "Lock Zoom";
    case Action::LockRotate: return "Lock Rotation";
    case Action::LockView: return "Lock View (All)";
    case Action::ToggleFullscreen: return "Borderless Fullscreen";
    case Action::SaveProject: return "Save Project";
    case Action::SaveProjectAs: return "Save Project As...";
    case Action::ExportImage: return "Export Image";
    case Action::ExportImageAs: return "Export Image As...";
    case Action::ToggleBeforeAfter: return "Before / After Split";
    case Action::ToggleErase: return "Erase Mode";
    default: return "?";
    }
}

void describe(Action action, char *buf, size_t size)
{
    const Binding &b = binding(action);
    if (size == 0)
        return;
    buf[0] = '\0';
    if (b.ctrl)  strncat(buf, "Ctrl+", size - 1);
    if (b.shift) strncat(buf, "Shift+", size - 1);
    if (b.alt)   strncat(buf, "Alt+", size - 1);

    const char *kname = ImGui::GetKeyName(b.key);
    if (is_shift_key(b.key)) kname = "Shift";
    else if (is_ctrl_key(b.key)) kname = "Ctrl";
    else if (is_alt_key(b.key)) kname = "Alt";
    strncat(buf, kname, size - 1);
}

uint32_t imgui_key_to_vk(ImGuiKey k)
{
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return static_cast<uint32_t>('0' + (k - ImGuiKey_0));
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return static_cast<uint32_t>('A' + (k - ImGuiKey_A));
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return static_cast<uint32_t>(VK_F1 + (k - ImGuiKey_F1));
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return static_cast<uint32_t>(VK_NUMPAD0 + (k - ImGuiKey_Keypad0));

    switch (k) {
    case ImGuiKey_LeftShift:  return VK_LSHIFT;
    case ImGuiKey_RightShift: return VK_RSHIFT;
    case ImGuiKey_LeftCtrl:   return VK_LCONTROL;
    case ImGuiKey_RightCtrl:  return VK_RCONTROL;
    case ImGuiKey_LeftAlt:    return VK_LMENU;
    case ImGuiKey_RightAlt:   return VK_RMENU;
    case ImGuiKey_Tab:        return VK_TAB;
    case ImGuiKey_LeftArrow:  return VK_LEFT;
    case ImGuiKey_RightArrow: return VK_RIGHT;
    case ImGuiKey_UpArrow:    return VK_UP;
    case ImGuiKey_DownArrow:  return VK_DOWN;
    case ImGuiKey_PageUp:     return VK_PRIOR;
    case ImGuiKey_PageDown:   return VK_NEXT;
    case ImGuiKey_Home:       return VK_HOME;
    case ImGuiKey_End:        return VK_END;
    case ImGuiKey_Insert:     return VK_INSERT;
    case ImGuiKey_Delete:     return VK_DELETE;
    case ImGuiKey_Backspace:  return VK_BACK;
    case ImGuiKey_Space:      return VK_SPACE;
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter: return VK_RETURN;
    case ImGuiKey_Escape:     return VK_ESCAPE;
    case ImGuiKey_LeftSuper:  return VK_LWIN;
    case ImGuiKey_RightSuper: return VK_RWIN;
    case ImGuiKey_Apostrophe: return VK_OEM_7;
    case ImGuiKey_Comma:      return VK_OEM_COMMA;
    case ImGuiKey_Minus:      return VK_OEM_MINUS;
    case ImGuiKey_Period:     return VK_OEM_PERIOD;
    case ImGuiKey_Slash:      return VK_OEM_2;
    case ImGuiKey_Semicolon:  return VK_OEM_1;
    case ImGuiKey_Equal:      return VK_OEM_PLUS;
    case ImGuiKey_LeftBracket:  return VK_OEM_4;
    case ImGuiKey_Backslash:    return VK_OEM_5;
    case ImGuiKey_RightBracket: return VK_OEM_6;
    case ImGuiKey_GraveAccent:  return VK_OEM_3;
    default:                  return 0;
    }
}

uint32_t to_vk(Action action)
{
    return imgui_key_to_vk(key(action));
}

uint32_t fine_tune_vk()
{
    uint32_t vk = to_vk(Action::FineTune);
    return vk != 0 ? vk : VK_SHIFT;
}

void sync_to_ipc(SharedControlBlock *block)
{
    if (!block) {
        block = JobQueueManager::get().get_control_block();
    }
    if (!block) return;

    for (int i = 0; i < static_cast<int>(Action::Count) && i < static_cast<int>(kMaxKeybinds); ++i) {
        Action a = static_cast<Action>(i);
        const Binding &b = binding(a);
        block->keybind_table[i].vk = to_vk(a);
        block->keybind_table[i].ctrl = b.ctrl ? 1 : 0;
        block->keybind_table[i].shift = b.shift ? 1 : 0;
        block->keybind_table[i].alt = b.alt ? 1 : 0;
        block->keybind_table[i].pad = 0;
    }
    block->fine_tune_vk = fine_tune_vk();
    block->keybind_version++;
}

} // namespace keybinds
