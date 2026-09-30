#define ImTextureID ImU64
#ifndef UNDOREDO_WINDOW_LAYOUT
#define UNDOREDO_WINDOW_LAYOUT 0
#endif
#include <imgui.h>
#define GImGui (ImGui::GetCurrentContext())
#include <imgui_internal.h>
#include <reshade.hpp>

namespace ImGui {
inline ImGuiContext* GetCurrentContext() {
    const auto *table = imgui_function_table_instance();
    if (!table || !table->GetIO) return nullptr;
    uintptr_t io_addr = reinterpret_cast<uintptr_t>(&table->GetIO());
    if (io_addr < 0x10000 || io_addr > 0x7FFFFFFFFFFF) return nullptr;
    uintptr_t ctx_addr = io_addr - offsetof(ImGuiContext, IO);
    if (ctx_addr < 0x10000 || ctx_addr > 0x7FFFFFFFFFFF) return nullptr;
    ImGuiContext *ctx = reinterpret_cast<ImGuiContext *>(ctx_addr);
    return ctx->Initialized ? ctx : nullptr;
}
}

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include <shellapi.h>

#include "ReShadeUndoRedo_API.h"
#include "DynamicPayload.hpp"
#include "ICommand.hpp"
#include "Commands.hpp"
#include "CommandQueue.hpp"
#include "ContextManager.hpp"
#include "AuditionStateMachine.hpp"
#include "TransactionCoalescer.hpp"
#include "AddonRegistry.hpp"

extern "C" __declspec(dllexport) const char *NAME = "Undo/Redo by NotRayST";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Undo and redo support for ReShade effects and presets";

reshade::api::effect_runtime *g_runtime = nullptr;

namespace {

std::string g_preset_path;
bool g_allow_all_hidden = false;
bool g_log_addon_uniforms = false;
bool g_global_mode = true;
bool g_standalone_tab = true;
char g_filter[128] = "";
int g_suppress_tracking_frames = 0;

bool matches_filter(const std::string &text, const char *f) {
    if (!f || !f[0]) return true;
    size_t flen = strlen(f);
    auto it = std::search(
        text.begin(), text.end(),
        f, f + flen,
        [](char ch1, char ch2) { return ::tolower(static_cast<unsigned char>(ch1)) == ::tolower(static_cast<unsigned char>(ch2)); }
    );
    return it != text.end();
}

// --- small ui helpers ---

struct ScopedDisabled {
    explicit ScopedDisabled(bool d) { ImGui::BeginDisabled(d); }
    ~ScopedDisabled() { ImGui::EndDisabled(); }
};

// dims text (theme's disabled color) while in scope, no-op when off
struct Dim {
    bool on;
    explicit Dim(bool o) : on(o) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    ~Dim() { if (on) ImGui::PopStyleColor(); }
};

// dimmed, wrapped helper text
void note(const char *fmt, ...) {
    char buf[320];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Dim dim(true);
    ImGui::TextWrapped("%s", buf);
}

void tip(const char *text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// --- uniform & technique filtering ---

bool is_trackable_uniform(reshade::api::effect_runtime *rt,
                          reshade::api::effect_uniform_variable var, bool allow_hidden)
{
    if (var == reshade::api::effect_uniform_variable{ 0 }) return false;

    char source[64] = {};
    if (rt->get_annotation_string_from_uniform_variable(var, "source", source) && source[0]) {
        std::string s = source;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
        static const char *excludes[] = {
            "framecount","frametime","time","date","timer","pingpong",
            "random","key","mousebutton","mousedelta","mousewheel","mousepoint","bufready_",
            "camera","overlay","menu","ui_"
        };
        for (auto e : excludes) if (s.find(e) != std::string::npos) return false;
    }

    char vn[256] = {};
    rt->get_uniform_variable_name(var, vn);
    if (is_ui_state_variable(vn)) return false;

    std::string vnl = vn;
    std::transform(vnl.begin(), vnl.end(), vnl.begin(), [](unsigned char c) { return (char)::tolower(c); });
    // blacklist automated camera streams from IGCS
    if (vnl.rfind("igcs_", 0) == 0) return false;

    char eff[256] = {};
    rt->get_uniform_variable_effect_name(var, eff);
    std::string enl = eff;
    std::transform(enl.begin(), enl.end(), enl.begin(), [](unsigned char c) { return (char)::tolower(c); });
    // blacklist multi-sample DoF helper passes
    if (enl.find("igcsdof") != std::string::npos) return false;

    int32_t noedit = 0;
    rt->get_annotation_int_from_uniform_variable(var, "noedit", &noedit, 1);
    if (noedit) return false;
    if (allow_hidden) return true;

    int32_t hidden = 0, nosave = 0;
    rt->get_annotation_int_from_uniform_variable(var, "hidden", &hidden, 1);
    rt->get_annotation_int_from_uniform_variable(var, "nosave", &nosave, 1);
    return !(hidden || nosave);
}

bool is_trackable_technique(reshade::api::effect_runtime *rt, reshade::api::effect_technique tech) {
    if (tech == reshade::api::effect_technique{ 0 }) return false;
    int32_t hidden = 0, force = 0, ss = 0, to = 0;
    rt->get_annotation_int_from_technique(tech, "hidden", &hidden, 1);
    rt->get_annotation_int_from_technique(tech, "enabled", &force, 1);
    rt->get_annotation_int_from_technique(tech, "enabled_in_screenshot", &ss, 1);
    rt->get_annotation_int_from_technique(tech, "timeout", &to, 1);
    return !(hidden || force || ss || to);
}

// --- keybinds ---

enum class Action { Undo = 0, Redo = 1, Count = 2 };

struct Binding { ImGuiKey key = ImGuiKey_None; bool ctrl = false, shift = false, alt = false; };

Binding g_defaults[] = {
    { ImGuiKey_Z, true, false, false },
    { ImGuiKey_Y, true, false, false },
};
Binding g_bindings[2];

bool is_mod(ImGuiKey k) {
    return k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl ||
           k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift ||
           k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt ||
           k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper;
}
bool is_ctrl(ImGuiKey k)  { return k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl; }
bool is_shift(ImGuiKey k) { return k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift; }
bool is_alt(ImGuiKey k)   { return k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt; }

const char *action_name(Action a) { return a == Action::Undo ? "Undo" : "Redo"; }

void describe_binding(Action a, char *buf, size_t sz) {
    const Binding &b = g_bindings[static_cast<int>(a)];
    buf[0] = '\0';
    if (b.ctrl)  strncat(buf, "Ctrl+", sz - 1);
    if (b.shift) strncat(buf, "Shift+", sz - 1);
    if (b.alt)   strncat(buf, "Alt+", sz - 1);
    const char *kn = ImGui::GetKeyName(b.key);
    if (is_shift(b.key)) kn = "Shift";
    else if (is_ctrl(b.key)) kn = "Ctrl";
    else if (is_alt(b.key)) kn = "Alt";
    strncat(buf, kn ? kn : "None", sz - 1);
}

static void binding_keys(Action a, char *kn, char *cn, char *sn, char *an) {
    const char *pfx = a == Action::Undo ? "Undo" : "Redo";
    snprintf(kn, 32, "%sKey", pfx); snprintf(cn, 32, "%sCtrl", pfx);
    snprintf(sn, 32, "%sShift", pfx); snprintf(an, 32, "%sAlt", pfx);
}

void load_binding(Action a) {
    auto &b = g_bindings[static_cast<int>(a)];
    b = g_defaults[static_cast<int>(a)];
    if (!g_runtime) return;
    char kn[32], cn[32], sn[32], an[32];
    binding_keys(a, kn, cn, sn, an);
    int kv = static_cast<int>(b.key);
    reshade::get_config_value(g_runtime, "UndoRedo", kn, kv); b.key = static_cast<ImGuiKey>(kv);
    reshade::get_config_value(g_runtime, "UndoRedo", cn, b.ctrl);
    reshade::get_config_value(g_runtime, "UndoRedo", sn, b.shift);
    reshade::get_config_value(g_runtime, "UndoRedo", an, b.alt);
}

void save_binding(Action a) {
    if (!g_runtime) return;
    const auto &b = g_bindings[static_cast<int>(a)];
    char kn[32], cn[32], sn[32], an[32];
    binding_keys(a, kn, cn, sn, an);
    reshade::set_config_value(g_runtime, "UndoRedo", kn, static_cast<int>(b.key));
    reshade::set_config_value(g_runtime, "UndoRedo", cn, b.ctrl);
    reshade::set_config_value(g_runtime, "UndoRedo", sn, b.shift);
    reshade::set_config_value(g_runtime, "UndoRedo", an, b.alt);
}

void init_keybinds(reshade::api::effect_runtime *rt) {
    g_runtime = rt;
    for (int i = 0; i < 2; ++i) load_binding(static_cast<Action>(i));
}

void reset_keybinds() {
    for (int i = 0; i < 2; ++i) { g_bindings[i] = g_defaults[i]; save_binding(static_cast<Action>(i)); }
}

int s_capturing = -1;
int s_cap_seen = 0;  // last frame the keybinding row was drawn

bool is_pressed(Action a) {
    const auto &b = g_bindings[static_cast<int>(a)];
    if (!ImGui::IsKeyPressed(b.key, false)) return false;
    ImGuiIO &io = ImGui::GetIO();
    if (b.ctrl != io.KeyCtrl && !is_ctrl(b.key)) return false;
    if (b.shift != io.KeyShift && !is_shift(b.key)) return false;
    if (b.alt != io.KeyAlt && !is_alt(b.key)) return false;
    return true;
}

// --- undo / redo dispatch ---

void flush_pending() {
    g_suppress_tracking_frames = 2;
    AuditionStateMachine::instance().flush_session();
    TransactionCoalescer::instance().commit_transaction();
}

void step(reshade::api::effect_runtime *rt, bool redo) {
    flush_pending();
    if (g_global_mode)
        redo ? ContextManager::instance().redo_global(rt) : ContextManager::instance().undo_global(rt);
    else
        redo ? ContextManager::instance().redo_active(rt) : ContextManager::instance().undo_active(rt);
    if (rt) rt->save_current_preset();
}

// --- reshade event handlers ---

bool on_set_uniform_value(reshade::api::effect_runtime *rt,
                          reshade::api::effect_uniform_variable variable,
                          const void *new_value, size_t new_value_size)
{
    if (ContextManager::instance().is_suppressing()) return false;
    if (!is_trackable_uniform(rt, variable, g_allow_all_hidden)) return false;

    reshade::api::format base_type = reshade::api::format::unknown;
    uint32_t rows = 0, cols = 0, arr = 0;
    rt->get_uniform_variable_type(variable, &base_type, &rows, &cols, &arr);

    char eff[256] = {}, vn[256] = {};
    rt->get_uniform_variable_effect_name(variable, eff);
    rt->get_uniform_variable_name(variable, vn);

    uint32_t elem = (std::max)(1u, rows) * (std::max)(1u, cols) * (std::max)(1u, arr);

    DynamicPayload before;
    before.format = base_type; before.rows = rows; before.cols = cols; before.element_count = elem;

    switch (base_type) {
    case reshade::api::format::r32_typeless:
        before.bytes.resize(elem * sizeof(bool));
        rt->get_uniform_value_bool(variable, reinterpret_cast<bool *>(before.bytes.data()), elem); break;
    case reshade::api::format::r32_float:
        before.bytes.resize(elem * sizeof(float));
        rt->get_uniform_value_float(variable, reinterpret_cast<float *>(before.bytes.data()), elem); break;
    case reshade::api::format::r32_sint:
        before.bytes.resize(elem * sizeof(int32_t));
        rt->get_uniform_value_int(variable, reinterpret_cast<int32_t *>(before.bytes.data()), elem); break;
    case reshade::api::format::r32_uint:
        before.bytes.resize(elem * sizeof(uint32_t));
        rt->get_uniform_value_uint(variable, reinterpret_cast<uint32_t *>(before.bytes.data()), elem); break;
    default: return false;
    }

    DynamicPayload after = before;

    if (base_type == reshade::api::format::r32_typeless) {
        if (new_value_size == elem * sizeof(uint32_t) || (new_value_size % 4 == 0 && new_value_size > after.bytes.size())) {
            size_t cnt = (std::min)(elem, static_cast<uint32_t>(new_value_size / 4));
            const uint32_t *w = static_cast<const uint32_t *>(new_value);
            for (size_t i = 0; i < cnt; ++i) after.bytes[i] = w[i] ? 1 : 0;
        } else {
            size_t cnt = (std::min)(after.bytes.size(), new_value_size);
            const uint8_t *s = static_cast<const uint8_t *>(new_value);
            for (size_t i = 0; i < cnt; ++i) after.bytes[i] = s[i] ? 1 : 0;
        }
    } else {
        std::memcpy(after.bytes.data(), new_value, (std::min)(after.bytes.size(), new_value_size));
    }

    if (before == after) return false;

    bool mouse = is_user_interacting();
    if (g_log_addon_uniforms) {
        char msg[512] = {};
        snprintf(msg, sizeof(msg),
            "[UndoRedo] Intercepted: Effect='%s', Var='%s', Elements=%u, Bytes=%zu, Mouse=%d",
            eff, vn, elem, new_value_size, mouse ? 1 : 0);
        reshade::log::message(reshade::log::level::info, msg);
    }

    if (mouse) {
        TransactionCoalescer::instance().stage_uniform(eff, vn, std::move(before), std::move(after));
    } else {
        ContextManager::instance().push_command(
            std::make_unique<UniformCommand>(eff, vn, std::move(before), std::move(after)));
    }
    return false;
}

bool on_set_technique_state(reshade::api::effect_runtime *rt,
                            reshade::api::effect_technique tech, bool enabled)
{
    if (ContextManager::instance().is_suppressing()) return false;
    if (!is_trackable_technique(rt, tech)) return false;
    char eff[256] = {}, tn[128] = {};
    rt->get_technique_effect_name(tech, eff);
    rt->get_technique_name(tech, tn);
    AuditionStateMachine::instance().process_technique_toggle(eff, tn, enabled);
    return false;
}

void on_preset_changed(reshade::api::effect_runtime *, const char *path) {
    std::string np = path ? path : "";
    if (np == g_preset_path) return;
    g_preset_path = np;
    AuditionStateMachine::instance().reset();
    TransactionCoalescer::instance().reset();
    ContextManager::instance().clear_all();
}

void dbg(const char *fmt, ...) {
    if (!g_log_addon_uniforms) return;
    char buf[256];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    reshade::log::message(reshade::log::level::info, buf);
}

void track_overlay_windows() {
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (!ctx) return;

    struct WinTrack {
        ImVec2 cpos, csize; uint32_t cdock = 0;
        ImVec2 dpos, dsize; uint32_t ddock = 0;
        bool dragging = false, has_base = false;
    };
    static std::unordered_map<ImGuiID, WinTrack> s_tracked;

    static int s_suppress_frames = 0;
    if (g_suppress_tracking_frames > 0) {
        s_suppress_frames = g_suppress_tracking_frames;
        g_suppress_tracking_frames = 0;
    }
    bool suppressing = ContextManager::instance().is_suppressing() || WindowTransformCommand::s_defer_transforms;
    if (s_suppress_frames > 0) {
        --s_suppress_frames;
        suppressing = true;
    }

    bool mouse_down = ctx->IO.MouseDown[0] || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);

    static ImVec2 s_last_disp = {};
    bool res_changed = s_last_disp.x > 0 && s_last_disp.y > 0 &&
        (fabs(ctx->IO.DisplaySize.x - s_last_disp.x) >= 1.0f || fabs(ctx->IO.DisplaySize.y - s_last_disp.y) >= 1.0f);
    s_last_disp = ctx->IO.DisplaySize;

    ImGuiDockNode *reshade_node = nullptr;
    for (ImGuiWindow *w : ctx->Windows)
        if (w && w->DockNode && is_reshade_main_dock_node(w->DockNode)) { reshade_node = w->DockNode; break; }

    struct Target {
        const char *name;
        ImGuiID id;
        ImVec2 pos, size;
        uint32_t dock;
        bool track_ps;
    };
    std::vector<Target> targets;

    if (reshade_node)
        targets.push_back({"ReShade", reshade_node->ID, reshade_node->Pos, reshade_node->Size, reshade_node->ID, true});

    for (ImGuiWindow *w : ctx->Windows) {
        if (!w || !w->Name || !w->Name[0] || w->IsExplicitChild) continue;
        if (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) continue;
        if (strncmp(w->Name, "##", 2) == 0 && !strstr(w->Name, "###")) continue;
        if (strcmp(w->Name, "Viewport") == 0 || strcmp(w->Name, "##Viewport") == 0) continue;
        if (!w->WasActive && !w->Active) continue;

        bool is_child_docked = (w->DockNode != nullptr) &&
                               !(w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size <= 1);
        uint32_t dock_id = is_child_docked ? w->DockNode->ID : 0;
        targets.push_back({w->Name, w->ID, w->Pos, w->Size, dock_id, !is_child_docked});
    }

    auto ps_changed = [](const Target &t, const ImVec2 &pos, const ImVec2 &size, bool &pc, bool &sc) {
        pc = t.track_ps && (fabs(t.pos.x - pos.x) >= 1.0f || fabs(t.pos.y - pos.y) >= 1.0f);
        sc = t.track_ps && (fabs(t.size.x - size.x) >= 1.0f || fabs(t.size.y - size.y) >= 1.0f);
        return pc || sc;
    };

    for (const auto &t : targets) {
        auto &st = s_tracked[t.id];
        bool pc = false, sc = false;
        bool changed = st.has_base && ps_changed(t, st.cpos, st.csize, pc, sc);

        if (!st.has_base || suppressing || res_changed) {
            if (changed && mouse_down)
                dbg("[UndoRedo] DROP '%s': suppress_ctx=%d defer=%d res=%d",
                    t.name, ContextManager::instance().is_suppressing(),
                    WindowTransformCommand::s_defer_transforms, res_changed);
            st.cpos = t.pos; st.csize = t.size; st.cdock = t.dock;
            st.dpos = t.pos; st.dsize = t.size; st.ddock = t.dock;
            st.dragging = false; st.has_base = true;
            continue;
        }

        if (mouse_down && changed && !st.dragging) {
            dbg("[UndoRedo] START '%s'", t.name);
            st.dragging = true;
            st.dpos = st.cpos; st.dsize = st.csize; st.ddock = st.cdock;
        }
    }

    if (!mouse_down) {
        for (const auto &t : targets) {
            auto &st = s_tracked[t.id];
            bool pc = false, sc = false;

            if (!st.dragging) {
                if (ps_changed(t, st.cpos, st.csize, pc, sc)) {
                    dbg("[UndoRedo] MISS '%s': changed with mouse up, never seen mid-drag", t.name);
                    st.cpos = t.pos; st.csize = t.size; st.cdock = t.dock;
                }
                continue;
            }

            st.dragging = false;
            if (ps_changed(t, st.dpos, st.dsize, pc, sc)) {
                dbg("[UndoRedo] COMMIT '%s'", t.name);
                float bp[2] = {st.dpos.x, st.dpos.y}, bs[2] = {st.dsize.x, st.dsize.y};
                float ap[2] = {t.pos.x, t.pos.y}, as[2] = {t.size.x, t.size.y};
                ContextManager::instance().push_command(std::make_unique<WindowTransformCommand>(
                    t.name, bp, bs, ap, as, -1, -1, st.ddock, t.dock));
            }
            st.cpos = t.pos; st.csize = t.size; st.cdock = t.dock;
        }
    }
}

void on_overlay_frame(reshade::api::effect_runtime *rt) {
    if (!ImGui::GetCurrentContext()) return;
#if UNDOREDO_WINDOW_LAYOUT
    if (WindowTransformCommand::s_defer_transforms && !ImGui::IsAnyItemActive()) {
        WindowTransformCommand::set_defer_transforms(false);
        g_suppress_tracking_frames = 2;
    }
    track_overlay_windows();
#endif
    TransactionCoalescer::instance().update_frame();
    AuditionStateMachine::instance().update_frame();

    ImGuiIO &io = ImGui::GetIO();
    // panel hidden mid-capture (tab switch, collapsed header): drop the capture or hotkeys stay dead
    if (s_capturing != -1 && ImGui::GetFrameCount() > s_cap_seen + 2) s_capturing = -1;
    if (io.WantTextInput || s_capturing != -1) return;

    if (is_pressed(Action::Undo)) step(rt, false);
    else if (is_pressed(Action::Redo)) step(rt, true);
    else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) step(rt, true);
}

void on_runtime_init(reshade::api::effect_runtime *rt) {
    g_runtime = rt;
    init_keybinds(rt);
    reshade::get_config_value(rt, "UndoRedo", "CaptureAllHiddenUniforms", g_allow_all_hidden);
    reshade::get_config_value(rt, "UndoRedo", "LogAddonUniforms", g_log_addon_uniforms);
    reshade::get_config_value(rt, "UndoRedo", "GlobalUndoMode", g_global_mode);
    reshade::get_config_value(rt, "UndoRedo", "StandaloneTab", g_standalone_tab);
    reshade::set_config_value(rt, "INPUT", "InputProcessing", 1);

    char pp[MAX_PATH] = {}; size_t ps = sizeof(pp);
    rt->get_current_preset_path(pp, &ps);
    g_preset_path = pp;
}

void on_runtime_destroy(reshade::api::effect_runtime *rt) {
    if (g_runtime == rt) g_runtime = nullptr;
}

// --- overlay ui ---

// newest-first rows; only visible rows are built when unfiltered
template <class Match, class Row>
void draw_rows(size_t n, Match match, Row row) {
    if (g_filter[0]) {
        for (size_t i = n; i-- > 0;) if (match(i)) row(i);
        return;
    }
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(n));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) row(n - 1 - static_cast<size_t>(r));
}

bool initial_row(bool at_base) {
    Dim dim(!at_base);
    return ImGui::Selectable("Initial state", at_base);
}

void render_queue_viewer(reshade::api::effect_runtime *rt, CommandDomain domain, const char *child_id) {
    ContextManager &cm = ContextManager::instance();
    CommandQueue &q = cm.get_domain(domain);
    if (q.size() == 0) {
        note("No edits yet. Changes you make will show up here.");
        return;
    }

    ImGui::BeginChild(child_id, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (initial_row(q.undo_count() == 0)) {
        flush_pending(); cm.jump_domain(domain, q.size(), rt);
    }

    const auto &entries = q.entries();
    draw_rows(entries.size(),
        [&](size_t i) { return matches_filter(entries[i]->get_label(), g_filter); },
        [&](size_t i) {
            std::string label = entries[i]->get_label();
            std::string delta = entries[i]->get_delta_preview();
            bool undone = q.is_undone(i);
            Dim dim(undone);
            ImGui::PushID(static_cast<int>(i));
            std::string txt = delta.empty() ? label : label + "  [" + delta + "]";
            if (ImGui::Selectable(txt.c_str(), !undone && i == q.redo_count())) {
                flush_pending(); cm.jump_domain(domain, i, rt);
            }
            ImGui::PopID();
        });
    ImGui::EndChild();
}

void render_combined_timeline(reshade::api::effect_runtime *rt) {
    ContextManager &cm = ContextManager::instance();
    const auto &tl = cm.global_timeline();
    if (tl.empty()) {
        note("No edits yet. Changes you make will show up here.");
        return;
    }

    ImGui::BeginChild("CombinedTimelineChild", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (initial_row(cm.global_undo_count() == 0)) {
        flush_pending(); cm.jump_global(tl.size(), rt);
    }

    draw_rows(tl.size(),
        [&](size_t i) { return matches_filter(tl[i].label, g_filter); },
        [&](size_t i) {
            const char *tag = tl[i].domain == CommandDomain::WindowLayout ? "[UI] "
                            : tl[i].domain == CommandDomain::AddonCustom  ? "[EXT] " : "[FX] ";
            bool undone = cm.global_undone(i);
            Dim dim(undone);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable((std::string(tag) + tl[i].label).c_str(), !undone && i == cm.global_redo_count())) {
                flush_pending(); cm.jump_global(i, rt);
            }
            ImGui::PopID();
        });
    ImGui::EndChild();
}

void draw_keybindings() {
    note("Click a binding, then press the new key combination. Ctrl+Shift+Z also redoes.");
    ImGui::Spacing();

    const float fs = ImGui::GetFontSize(), col_x = fs * 5.0f, btn_w = fs * 9.0f;

    struct ChordState { bool ctrl = false, shift = false, alt = false; ImGuiKey normal = ImGuiKey_None, last_mod = ImGuiKey_None;
        bool empty() const { return !ctrl && !shift && !alt && normal == ImGuiKey_None; }
        int count() const { return (ctrl?1:0) + (shift?1:0) + (alt?1:0) + (normal != ImGuiKey_None?1:0); }
    };
    static int s_cap_frame = 0;
    static ChordState s_peak; static bool s_has_peak = false;
    static uint64_t s_rel_ms = 0; static bool s_in_rel = false;
    static bool s_pc = false, s_ps = false, s_pa = false;
    static ImGuiKey s_lm = ImGuiKey_None;

    for (int a = 0; a < 2; ++a) {
        Action act = static_cast<Action>(a);
        ImGui::Text("%s", action_name(act));
        ImGui::SameLine(col_x);

        if (s_capturing == a) {
            s_cap_seen = ImGui::GetFrameCount();
            if (ImGui::GetFrameCount() > s_cap_frame + 2) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) s_capturing = -1;
                else if (ImGui::IsMouseClicked(0, false) && !s_has_peak) s_capturing = -1;
            }
            if (s_capturing == a) {
                ChordState cur;
                cur.ctrl  = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) || ImGui::GetIO().KeyCtrl;
                cur.shift = ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0) || ImGui::GetIO().KeyShift;
                cur.alt   = ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0) || ImGui::GetIO().KeyAlt;
                if (cur.ctrl && !s_pc)   s_lm = (GetAsyncKeyState(VK_RCONTROL)&0x8000) ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
                if (cur.shift && !s_ps)  s_lm = (GetAsyncKeyState(VK_RSHIFT)&0x8000) ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
                if (cur.alt && !s_pa)    s_lm = (GetAsyncKeyState(VK_RMENU)&0x8000) ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
                s_pc = cur.ctrl; s_ps = cur.shift; s_pa = cur.alt;

                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                    ImGuiKey ik = static_cast<ImGuiKey>(k);
                    if (ik == ImGuiKey_Escape || (ik >= ImGuiKey_MouseLeft && ik <= ImGuiKey_MouseMiddle) ||
                        is_mod(ik) || (ik >= ImGuiKey_ReservedForModCtrl && ik <= ImGuiKey_ReservedForModSuper)) continue;
                    if (ImGui::IsKeyDown(ik)) { cur.normal = ik; break; }
                }
                cur.last_mod = s_lm;

                uint64_t now = GetTickCount64();
                int cc = cur.count(), pc = s_has_peak ? s_peak.count() : 0;
                if (cc > 0) {
                    if (cc >= pc || (cur.normal != ImGuiKey_None && s_peak.normal == ImGuiKey_None)) {
                        s_peak = cur; s_has_peak = true; s_in_rel = false;
                    } else if (!s_in_rel) { s_in_rel = true; s_rel_ms = now; }
                    else if (now - s_rel_ms > 250) { s_peak = cur; s_in_rel = false; }
                }

                int dots = 1 + static_cast<int>(fmod(ImGui::GetTime() * 3, 3));
                std::string preview;
                const ChordState &disp = !cur.empty() ? cur : s_peak;
                if (!disp.empty() && (s_has_peak || !cur.empty())) {
                    if (disp.ctrl)  preview += "Ctrl+";
                    if (disp.shift) preview += "Shift+";
                    if (disp.alt)   preview += "Alt+";
                    if (disp.normal != ImGuiKey_None) preview += ImGui::GetKeyName(disp.normal);
                    else if (!cur.empty()) preview += std::string(dots, '.');
                    else if (disp.last_mod != ImGuiKey_None) preview += is_shift(disp.last_mod) ? "Shift" : is_ctrl(disp.last_mod) ? "Ctrl" : "Alt";
                } else preview = "Press a key" + std::string(dots, '.');

                ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", preview.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("Esc to cancel");

                // commit when all keys released
                if (s_has_peak && cur.empty() && ImGui::GetFrameCount() > s_cap_frame + 2) {
                    ImGuiKey pk = ImGuiKey_None; bool bc = false, bs = false, ba = false;
                    if (s_peak.normal != ImGuiKey_None) {
                        pk = s_peak.normal; bc = s_peak.ctrl; bs = s_peak.shift; ba = s_peak.alt;
                    } else if (s_peak.count() > 0) {
                        ImGuiKey m = s_peak.last_mod != ImGuiKey_None ? s_peak.last_mod :
                            s_peak.shift ? ImGuiKey_LeftShift : s_peak.ctrl ? ImGuiKey_LeftCtrl : ImGuiKey_LeftAlt;
                        pk = m;
                        if (!is_ctrl(m))  bc = s_peak.ctrl;
                        if (!is_shift(m)) bs = s_peak.shift;
                        if (!is_alt(m))   ba = s_peak.alt;
                    }
                    if (pk != ImGuiKey_None) {
                        g_bindings[a] = {pk, bc, bs, ba};
                        save_binding(act);
                    }
                    s_capturing = -1; s_peak = {}; s_has_peak = false;
                    s_in_rel = false; s_pc = s_ps = s_pa = false; s_lm = ImGuiKey_None;
                }
            }
        } else {
            char b[96]; describe_binding(act, b, sizeof(b));
            char id[104]; snprintf(id, sizeof(id), "%s###kb%d", b, a);
            if (ImGui::Button(id, ImVec2(btn_w, 0))) {
                s_capturing = a; s_cap_frame = s_cap_seen = ImGui::GetFrameCount();
                s_peak = {}; s_has_peak = false; s_in_rel = false;
                s_pc = s_ps = s_pa = false; s_lm = ImGuiKey_None;
            }
        }
    }

    const Binding &u = g_bindings[0], &r = g_bindings[1];
    if (u.key == r.key && u.ctrl == r.ctrl && u.shift == r.shift && u.alt == r.alt)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Undo and Redo share a binding. Redo won't trigger from it.");

    ImGui::Spacing();
    if (ImGui::Button("Reset keybinds to defaults")) reset_keybinds();
}

void on_draw_overlay(reshade::api::effect_runtime *rt) {
    try {
        ContextManager &cm = ContextManager::instance();

        ImGui::Spacing();
        ImGui::SeparatorText("History");

        size_t uc = g_global_mode ? cm.global_undo_count() : cm.get_domain(cm.active_domain()).undo_count();
        size_t rc = g_global_mode ? cm.global_redo_count() : cm.get_domain(cm.active_domain()).redo_count();
        note("Click an entry to jump. %zu undoable, %zu redoable (%s).", uc, rc, g_global_mode ? "all tabs" : "this tab");
        ImGui::Spacing();

        if (AuditionStateMachine::instance().is_active()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
            ImGui::TextWrapped("[Auditioning: %s - %.1fs remaining]",
                               AuditionStateMachine::instance().target_label().c_str(),
                               AuditionStateMachine::instance().time_remaining_sec());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        char ub[64], rb[64], ud[32], rd[32];
        describe_binding(Action::Undo, ud, sizeof(ud));
        describe_binding(Action::Redo, rd, sizeof(rd));
        snprintf(ub, sizeof(ub), "Undo (%s)###undo", ud);
        snprintf(rb, sizeof(rb), "Redo (%s)###redo", rd);

        float btn_w = (std::max)(60.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
        { ScopedDisabled off(uc == 0); if (ImGui::Button(ub, ImVec2(btn_w, 0))) step(rt, false); }
        ImGui::SameLine();
        { ScopedDisabled off(rc == 0); if (ImGui::Button(rb, ImVec2(btn_w, 0))) step(rt, true); }
        ImGui::Spacing();

        // timeline scrubber
        size_t total = g_global_mode ? cm.global_size() : cm.get_domain(cm.active_domain()).size();
        if (total > 0) {
            int cur = static_cast<int>(uc), mx = static_cast<int>(total);
            char sfmt[48]; snprintf(sfmt, sizeof(sfmt), "Step %%d of %d", mx);
            ImGui::SetNextItemWidth(-1.0f);
            bool scrub_changed = ImGui::SliderInt("##HistoryScrub", &cur, 0, mx, sfmt);
            bool scrub_active = ImGui::IsItemActive();
            bool scrub_deactivated = ImGui::IsItemDeactivated();

            if (scrub_active) {
                WindowTransformCommand::set_defer_transforms(true);
                g_suppress_tracking_frames = 2;
            }

            if (scrub_changed) {
                flush_pending();
                size_t tp = static_cast<size_t>(mx - cur);
                g_global_mode ? cm.jump_global(tp, rt) : cm.jump_domain(cm.active_domain(), tp, rt);
            }

            if (scrub_deactivated || (!scrub_active && WindowTransformCommand::has_deferred_transforms())) {
                WindowTransformCommand::set_defer_transforms(false);
                g_suppress_tracking_frames = 2;
            }
            ImGui::Spacing();
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##HistoryFilter", "Filter history...", g_filter, sizeof(g_filter));
        ImGui::Spacing();

        if (ImGui::BeginTabBar("UndoRedoTabs")) {
            if (ImGui::BeginTabItem("Shaders")) {
                cm.set_active_domain(CommandDomain::ShaderState);
                render_queue_viewer(rt, CommandDomain::ShaderState, "ShaderHistoryChild"); ImGui::EndTabItem(); }
#if UNDOREDO_WINDOW_LAYOUT
            if (ImGui::BeginTabItem("Window Layout")) {
                cm.set_active_domain(CommandDomain::WindowLayout);
                render_queue_viewer(rt, CommandDomain::WindowLayout, "WindowHistoryChild"); ImGui::EndTabItem(); }
#endif
            if (ImGui::BeginTabItem("Combined Timeline")) {
                render_combined_timeline(rt); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        {
            ScopedDisabled off(cm.global_size() == 0);
            if (ImGui::Button("Clear all history")) ImGui::OpenPopup("Clear history?");
        }
        if (ImGui::BeginPopupModal("Clear history?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Remove all %zu entries from every tab? This can't be undone.", cm.global_size());
            if (ImGui::Button("Clear")) { cm.clear_all(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Settings")) {
            if (ImGui::Checkbox("Show as its own ReShade tab", &g_standalone_tab)) {
                if (rt) reshade::set_config_value(rt, "UndoRedo", "StandaloneTab", g_standalone_tab);
                reshade::unregister_overlay(g_standalone_tab ? nullptr : "Undo / Redo", &on_draw_overlay);
                reshade::register_overlay(g_standalone_tab ? "Undo / Redo" : nullptr, &on_draw_overlay);
            }
            tip("When enabled, registers as a dedicated top-level tab in ReShade alongside Home, Settings, and Statistics.\nWhen disabled, docks inside ReShade's Add-ons tab.");

            if (ImGui::Checkbox("Capture all hidden uniforms", &g_allow_all_hidden))
                if (rt) reshade::set_config_value(rt, "UndoRedo", "CaptureAllHiddenUniforms", g_allow_all_hidden);
            tip("By default, hidden and nosave uniforms are excluded.\nEnabling this captures hidden uniform changes across all shaders.");

            if (ImGui::Checkbox("Log captured uniforms to ReShade.log", &g_log_addon_uniforms))
                if (rt) reshade::set_config_value(rt, "UndoRedo", "LogAddonUniforms", g_log_addon_uniforms);
            tip("Logs every uniform change captured by Undo/Redo into ReShade.log for diagnostic analysis.");

            if (ImGui::Checkbox("Hotkeys use combined history", &g_global_mode))
                if (rt) reshade::set_config_value(rt, "UndoRedo", "GlobalUndoMode", g_global_mode);
            tip("When enabled, pressing Undo/Redo acts on the latest global action across all tabs.\nWhen disabled, acts only on the currently active tab.");
        }

        if (ImGui::CollapsingHeader("Keybindings")) draw_keybindings();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("Undo / Redo v1.1.0 by NotRayST");

        if (ImGui::Button("GitHub"))
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab", nullptr, nullptr, SW_SHOW);
        tip("https://github.com/NotRayST/ShaderLab");
        ImGui::SameLine();
        if (ImGui::Button("Support on Patreon"))
            ShellExecuteW(nullptr, L"open", L"https://www.patreon.com/cw/RayST", nullptr, nullptr, SW_SHOW);
        tip("https://www.patreon.com/cw/RayST");

    } catch (...) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error rendering Undo/Redo overlay");
    }
}

} // namespace

// --- inter-addon c abi ---

extern "C" {

__declspec(dllexport) bool ReShadeUndoRedo_RegisterAddon(const char *id, ReShadeUndoRedo_ActionHandler h, void *ud) {
    return AddonRegistry::instance().register_addon(id, h, ud);
}

__declspec(dllexport) void ReShadeUndoRedo_UnregisterAddon(const char *id) {
    AddonRegistry::instance().unregister_addon(id);
}

__declspec(dllexport) bool ReShadeUndoRedo_PushCustomAction(
    const char *id, const char *tag, const char *label,
    const uint8_t *before, const uint8_t *after, uint32_t sz)
{
    if (!id || !tag) return false;
    std::vector<uint8_t> bv, av;
    if (before && sz) bv.assign(before, before + sz);
    if (after && sz)  av.assign(after, after + sz);
    return ContextManager::instance().push_command(
        std::make_unique<ExternalAddonCommand>(id, tag, label ? label : "", std::move(bv), std::move(av)));
}

__declspec(dllexport) bool ReShadeUndoRedo_PushWindowTransform(
    const char *name, const float bp[2], const float bs[2], const float ap[2], const float as[2])
{
#if !UNDOREDO_WINDOW_LAYOUT
    (void)name; (void)bp; (void)bs; (void)ap; (void)as;
    return false;
#else
    if (!name || !bp || !bs || !ap || !as) return false;
    return ContextManager::instance().push_command(
        std::make_unique<WindowTransformCommand>(name, bp, bs, ap, as));
#endif
}

} // extern "C"

// --- addon lifecycle ---

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) return FALSE;
        init_keybinds(nullptr);
        reshade::get_config_value(nullptr, "UndoRedo", "StandaloneTab", g_standalone_tab);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_runtime_destroy);
        reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
        reshade::register_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
        reshade::register_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
        reshade::register_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
        reshade::register_overlay(g_standalone_tab ? "Undo / Redo" : nullptr, &on_draw_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(g_standalone_tab ? "Undo / Redo" : nullptr, &on_draw_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
        reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
        reshade::unregister_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
        reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_runtime_destroy);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
