#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <windows.h>
#include <shellapi.h>

extern "C" __declspec(dllexport) const char *NAME = "Undo/Redo by NotRayST";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Advanced Undo/Redo for ReShade";

namespace {

constexpr size_t kHistoryLimit = 1000;

struct HistoryEntry {
    enum class Kind {
        UniformValue,
        TechniqueState,
    };

    union Value {
        bool     as_bool;
        float    as_float[16];
        int32_t  as_int[16];
        uint32_t as_uint[16];
    };

    Kind kind = Kind::UniformValue;

    std::string effect_name;
    std::string variable_name;
    reshade::api::format base_type = reshade::api::format::unknown;
    Value before;
    Value after;

    std::string technique_name;
    bool technique_enabled = false;
};

struct History {
    std::list<HistoryEntry> entries;
    size_t position = 0;
    bool suppressing = false;
    std::string last_preset_path;
};

History &state()
{
    static History history;
    return history;
}

constexpr uint32_t kHistoryFileMagic = 0x53485421; // "SHT!"
constexpr uint32_t kHistoryFileVersion = 1;

std::wstring history_file_path()
{
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0)
        return L"ReShade_undo_history.dat";
    return std::wstring(temp) + L"ReShade_undo_history.dat";
}

void write_str(FILE *f, const std::string &s)
{
    uint32_t len = static_cast<uint32_t>(s.size());
    fwrite(&len, sizeof(len), 1, f);
    if (len > 0)
        fwrite(s.data(), 1, len, f);
}

bool read_str(FILE *f, std::string &out)
{
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1)
        return false;
    out.resize(len);
    if (len > 0 && fread(&out[0], 1, len, f) != len)
        return false;
    return true;
}

void save_history_to_file()
{
    const History &history = state();
    const std::wstring path = history_file_path();

    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f)
        return;

    fwrite(&kHistoryFileMagic, sizeof(kHistoryFileMagic), 1, f);
    fwrite(&kHistoryFileVersion, sizeof(kHistoryFileVersion), 1, f);

    uint64_t position = history.position;
    uint64_t count = history.entries.size();
    fwrite(&position, sizeof(position), 1, f);
    fwrite(&count, sizeof(count), 1, f);

    for (const HistoryEntry &e : history.entries) {
        uint32_t kind = static_cast<uint32_t>(e.kind);
        fwrite(&kind, sizeof(kind), 1, f);
        write_str(f, e.effect_name);

        if (e.kind == HistoryEntry::Kind::UniformValue) {
            write_str(f, e.variable_name);
            uint32_t base_type = static_cast<uint32_t>(e.base_type);
            fwrite(&base_type, sizeof(base_type), 1, f);
            fwrite(e.before.as_uint, sizeof(uint32_t), 16, f);
            fwrite(e.after.as_uint, sizeof(uint32_t), 16, f);
        } else {
            write_str(f, e.technique_name);
            uint8_t enabled = e.technique_enabled ? 1 : 0;
            fwrite(&enabled, sizeof(enabled), 1, f);
        }
    }

    fclose(f);
}

bool load_history_from_file()
{
    History &history = state();
    const std::wstring path = history_file_path();

    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;

    uint32_t magic = 0, version = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != kHistoryFileMagic ||
        fread(&version, sizeof(version), 1, f) != 1 || version != kHistoryFileVersion) {
        fclose(f);
        return false;
    }

    uint64_t position = 0, count = 0;
    if (fread(&position, sizeof(position), 1, f) != 1 ||
        fread(&count, sizeof(count), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (count > kHistoryLimit)
        count = kHistoryLimit;

    std::list<HistoryEntry> loaded;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t kind = 0;
        if (fread(&kind, sizeof(kind), 1, f) != 1)
            break;

        HistoryEntry e;
        e.kind = static_cast<HistoryEntry::Kind>(kind);
        if (!read_str(f, e.effect_name))
            break;

        if (e.kind == HistoryEntry::Kind::UniformValue) {
            if (!read_str(f, e.variable_name))
                break;
            uint32_t base_type = 0;
            if (fread(&base_type, sizeof(base_type), 1, f) != 1)
                break;
            e.base_type = static_cast<reshade::api::format>(base_type);
            if (fread(e.before.as_uint, sizeof(uint32_t), 16, f) != 16 ||
                fread(e.after.as_uint, sizeof(uint32_t), 16, f) != 16)
                break;
        } else {
            if (!read_str(f, e.technique_name))
                break;
            uint8_t enabled = 0;
            if (fread(&enabled, sizeof(enabled), 1, f) != 1)
                break;
            e.technique_enabled = enabled != 0;
        }

        loaded.push_back(std::move(e));
    }

    fclose(f);

    if (loaded.empty())
        return false;

    history.entries = std::move(loaded);
    history.position = std::min(static_cast<size_t>(position), history.entries.size());
    return true;
}

void delete_history_file()
{
    _wremove(history_file_path().c_str());
}

bool valid_uniform(reshade::api::effect_runtime *runtime, reshade::api::effect_uniform_variable variable)
{
    if (variable == reshade::api::effect_uniform_variable{ 0 })
        return false;

    char source[64] = {};
    if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source))
        return false;
    int32_t hidden = 0, noedit = 0, noreset = 0, nosave = 0;
    runtime->get_annotation_int_from_uniform_variable(variable, "hidden", &hidden, 1);
    runtime->get_annotation_int_from_uniform_variable(variable, "noedit", &noedit, 1);
    runtime->get_annotation_int_from_uniform_variable(variable, "noreset", &noreset, 1);
    runtime->get_annotation_int_from_uniform_variable(variable, "nosave", &nosave, 1);
    return !(hidden || noedit || noreset || nosave);
}

bool valid_technique(reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique)
{
    if (technique == reshade::api::effect_technique{ 0 })
        return false;

    int32_t hidden = 0, force = 0, screenshot = 0, timeout = 0;
    runtime->get_annotation_int_from_technique(technique, "hidden", &hidden, 1);
    runtime->get_annotation_int_from_technique(technique, "enabled", &force, 1);
    runtime->get_annotation_int_from_technique(technique, "enabled_in_screenshot", &screenshot, 1);
    runtime->get_annotation_int_from_technique(technique, "timeout", &timeout, 1);
    return !(hidden || force || screenshot || timeout);
}

void read_uniform_value(reshade::api::effect_runtime *runtime,
                        reshade::api::effect_uniform_variable variable,
                        reshade::api::format base_type,
                        HistoryEntry::Value &out)
{
    switch (base_type) {
    case reshade::api::format::r32_typeless:
        runtime->get_uniform_value_bool(variable, &out.as_bool, 1);
        break;
    case reshade::api::format::r32_float:
        runtime->get_uniform_value_float(variable, out.as_float, 16);
        break;
    case reshade::api::format::r32_sint:
        runtime->get_uniform_value_int(variable, out.as_int, 16);
        break;
    case reshade::api::format::r32_uint:
        runtime->get_uniform_value_uint(variable, out.as_uint, 16);
        break;
    default:
        break;
    }
}

void write_uniform_value(reshade::api::effect_runtime *runtime,
                         reshade::api::effect_uniform_variable variable,
                         reshade::api::format base_type,
                         const HistoryEntry::Value &value)
{
    switch (base_type) {
    case reshade::api::format::r32_typeless:
        runtime->set_uniform_value_bool(variable, &value.as_bool, 1);
        break;
    case reshade::api::format::r32_float:
        runtime->set_uniform_value_float(variable, value.as_float, 16);
        break;
    case reshade::api::format::r32_sint:
        runtime->set_uniform_value_int(variable, value.as_int, 16);
        break;
    case reshade::api::format::r32_uint:
        runtime->set_uniform_value_uint(variable, value.as_uint, 16);
        break;
    default:
        break;
    }
}

reshade::api::effect_uniform_variable resolve_uniform(reshade::api::effect_runtime *runtime,
                                                      const HistoryEntry &entry)
{
    auto u = runtime->find_uniform_variable(entry.effect_name.c_str(), entry.variable_name.c_str());
    if (u == reshade::api::effect_uniform_variable{ 0 }) {
        u = runtime->find_uniform_variable(nullptr, entry.variable_name.c_str());
    }
    return u;
}

reshade::api::effect_technique resolve_technique(reshade::api::effect_runtime *runtime,
                                                 const HistoryEntry &entry)
{
    auto tech = runtime->find_technique(entry.effect_name.c_str(), entry.technique_name.c_str());
    if (tech == reshade::api::effect_technique{ 0 }) {
        tech = runtime->find_technique(nullptr, entry.technique_name.c_str());
    }
    return tech;
}

void apply(reshade::api::effect_runtime *runtime, const HistoryEntry &entry, bool undo)
{
    switch (entry.kind) {
    case HistoryEntry::Kind::UniformValue: {
        const reshade::api::effect_uniform_variable variable = resolve_uniform(runtime, entry);
        if (variable == reshade::api::effect_uniform_variable{ 0 })
            return;
        write_uniform_value(runtime, variable, entry.base_type,
                            undo ? entry.before : entry.after);
        break;
    }
    case HistoryEntry::Kind::TechniqueState: {
        const reshade::api::effect_technique technique = resolve_technique(runtime, entry);
        if (technique == reshade::api::effect_technique{ 0 })
            return;
        runtime->set_technique_state(technique,
                                     undo ? !entry.technique_enabled : entry.technique_enabled);
        break;
    }
    }

    runtime->save_current_preset();
}

void record(History &history, HistoryEntry entry)
{
    while (history.position > 0) {
        history.entries.pop_front();
        --history.position;
    }

    // merge consecutive changes to the same variable
    if (entry.kind == HistoryEntry::Kind::UniformValue) {
        if (auto it = history.entries.begin(); it != history.entries.end() &&
            it->kind == HistoryEntry::Kind::UniformValue &&
            it->effect_name == entry.effect_name && it->variable_name == entry.variable_name) {
            std::memcpy(&entry.before, &it->before, sizeof(entry.before));
            history.entries.pop_front();
        }
    }

    if (history.entries.size() < kHistoryLimit)
        history.entries.push_front(std::move(entry));
    history.position = 0;
    save_history_to_file();
}

void undo(reshade::api::effect_runtime *runtime)
{
    History &history = state();
    if (history.position >= history.entries.size())
        return;

    auto it = history.entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(history.position));

    history.suppressing = true;
    apply(runtime, *it, true);
    ++history.position;
    history.suppressing = false;
    save_history_to_file();
}

void redo(reshade::api::effect_runtime *runtime)
{
    History &history = state();
    if (history.position == 0)
        return;

    auto it = history.entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(history.position) - 1);

    history.suppressing = true;
    --history.position;
    apply(runtime, *it, false);
    history.suppressing = false;
    save_history_to_file();
}

void jump_to_position(reshade::api::effect_runtime *runtime, size_t target_position)
{
    History &history = state();
    target_position = (std::min)(target_position, history.entries.size());

    while (history.position < target_position)
        undo(runtime);
    while (history.position > target_position)
        redo(runtime);
}

size_t undo_count() { return state().entries.size() - state().position; }
size_t redo_count() { return state().position; }
size_t history_size() { return state().entries.size(); }
bool history_undone(size_t index) { return index < state().position; }

bool history_label(size_t index, char *buf, size_t size)
{
    History &history = state();
    if (index >= history.entries.size() || buf == nullptr || size == 0)
        return false;

    auto it = history.entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));

    if (it->kind == HistoryEntry::Kind::UniformValue) {
        const char *scope = it->variable_name.c_str();
        if (scope[0] == '\0')
            scope = it->effect_name.c_str();
        switch (it->base_type) {
        case reshade::api::format::r32_typeless:
            snprintf(buf, size, "Uniform %s = %s", scope, it->after.as_bool ? "true" : "false");
            break;
        case reshade::api::format::r32_float:
            snprintf(buf, size, "Uniform %s = %.2f", scope, it->after.as_float[0]);
            break;
        case reshade::api::format::r32_sint:
            snprintf(buf, size, "Uniform %s = %d", scope, it->after.as_int[0]);
            break;
        case reshade::api::format::r32_uint:
            snprintf(buf, size, "Uniform %s = %u", scope, it->after.as_uint[0]);
            break;
        default:
            snprintf(buf, size, "Uniform %s", scope);
            break;
        }
    } else {
        snprintf(buf, size, "%s %s", it->technique_name.c_str(),
                 it->technique_enabled ? "on" : "off");
    }
    return true;
}

void clear_history()
{
    History &history = state();
    history.entries.clear();
    history.position = 0;
    save_history_to_file();
}

// --- keybinds & remapping ----------------------------------------------------

enum class Action {
    Undo = 0,
    Redo = 1,
    Count = 2
};

struct Binding {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

Binding g_defaults[static_cast<int>(Action::Count)] = {
    { ImGuiKey_Z, true, false, false }, // Undo (Ctrl+Z)
    { ImGuiKey_Y, true, false, false }, // Redo (Ctrl+Y)
};

Binding g_bindings[static_cast<int>(Action::Count)];
reshade::api::effect_runtime *g_runtime = nullptr;

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

const char *action_name(Action a)
{
    switch (a) {
    case Action::Undo: return "Undo";
    case Action::Redo: return "Redo";
    default: return "?";
    }
}

void describe_binding(Action a, char *buf, size_t size)
{
    const Binding &b = g_bindings[static_cast<int>(a)];
    if (size == 0) return;
    buf[0] = '\0';
    if (b.ctrl)  strncat(buf, "Ctrl+", size - 1);
    if (b.shift) strncat(buf, "Shift+", size - 1);
    if (b.alt)   strncat(buf, "Alt+", size - 1);

    const char *kname = ImGui::GetKeyName(b.key);
    if (is_shift_key(b.key)) kname = "Shift";
    else if (is_ctrl_key(b.key)) kname = "Ctrl";
    else if (is_alt_key(b.key)) kname = "Alt";
    strncat(buf, kname ? kname : "None", size - 1);
}

void load_binding(Action a)
{
    Binding &b = g_bindings[static_cast<int>(a)];
    b = g_defaults[static_cast<int>(a)];
    if (!g_runtime) return;

    const char *sec = "UndoRedo";
    const char *prefix = (a == Action::Undo) ? "Undo" : "Redo";
    char kName[32], cName[32], sName[32], aName[32];
    snprintf(kName, sizeof(kName), "%sKey", prefix);
    snprintf(cName, sizeof(cName), "%sCtrl", prefix);
    snprintf(sName, sizeof(sName), "%sShift", prefix);
    snprintf(aName, sizeof(aName), "%sAlt", prefix);

    int key_val = static_cast<int>(b.key);
    reshade::get_config_value(g_runtime, sec, kName, key_val);
    b.key = static_cast<ImGuiKey>(key_val);
    reshade::get_config_value(g_runtime, sec, cName, b.ctrl);
    reshade::get_config_value(g_runtime, sec, sName, b.shift);
    reshade::get_config_value(g_runtime, sec, aName, b.alt);
}

void save_binding(Action a)
{
    if (!g_runtime) return;
    const Binding &b = g_bindings[static_cast<int>(a)];
    const char *sec = "UndoRedo";
    const char *prefix = (a == Action::Undo) ? "Undo" : "Redo";
    char kName[32], cName[32], sName[32], aName[32];
    snprintf(kName, sizeof(kName), "%sKey", prefix);
    snprintf(cName, sizeof(cName), "%sCtrl", prefix);
    snprintf(sName, sizeof(sName), "%sShift", prefix);
    snprintf(aName, sizeof(aName), "%sAlt", prefix);

    reshade::set_config_value(g_runtime, sec, kName, static_cast<int>(b.key));
    reshade::set_config_value(g_runtime, sec, cName, b.ctrl);
    reshade::set_config_value(g_runtime, sec, sName, b.shift);
    reshade::set_config_value(g_runtime, sec, aName, b.alt);
}

void init_keybinds(reshade::api::effect_runtime *runtime)
{
    g_runtime = runtime;
    for (int i = 0; i < static_cast<int>(Action::Count); ++i)
        load_binding(static_cast<Action>(i));
}

void reset_keybinds()
{
    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
        g_bindings[i] = g_defaults[i];
        save_binding(static_cast<Action>(i));
    }
}

static int s_capturing = -1;

bool is_pressed(Action action)
{
    const Binding &b = g_bindings[static_cast<int>(action)];
    if (!ImGui::IsKeyPressed(b.key, false))
        return false;

    ImGuiIO &io = ImGui::GetIO();
    if (b.ctrl != io.KeyCtrl && !is_ctrl_key(b.key)) return false;
    if (b.shift != io.KeyShift && !is_shift_key(b.key)) return false;
    if (b.alt != io.KeyAlt && !is_alt_key(b.key)) return false;
    return true;
}

// --- reshade event handlers --------------------------------------------------

bool on_set_uniform_value(reshade::api::effect_runtime *runtime,
                          reshade::api::effect_uniform_variable variable,
                          const void *new_value, size_t new_value_size)
{
    History &history = state();
    if (history.suppressing || !valid_uniform(runtime, variable))
        return false;

    HistoryEntry entry;
    entry.kind = HistoryEntry::Kind::UniformValue;
    runtime->get_uniform_variable_type(variable, &entry.base_type);

    char eff[256] = {}, vn[256] = {};
    runtime->get_uniform_variable_effect_name(variable, eff);
    runtime->get_uniform_variable_name(variable, vn);
    entry.effect_name = eff;
    entry.variable_name = vn;

    std::memset(&entry.before, 0, sizeof(entry.before));
    std::memset(&entry.after, 0, sizeof(entry.after));
    read_uniform_value(runtime, variable, entry.base_type, entry.before);
    std::memcpy(entry.after.as_uint, new_value,
                (std::min)(new_value_size, sizeof(HistoryEntry::Value)));

    if (std::memcmp(&entry.before, &entry.after, sizeof(HistoryEntry::Value)) == 0)
        return false;

    record(history, std::move(entry));
    return false;
}

bool on_set_technique_state(reshade::api::effect_runtime *runtime,
                            reshade::api::effect_technique technique,
                            bool enabled)
{
    History &history = state();
    if (history.suppressing || !valid_technique(runtime, technique))
        return false;

    HistoryEntry entry;
    entry.kind = HistoryEntry::Kind::TechniqueState;
    entry.technique_enabled = enabled;

    char eff[256] = {}, tn[128] = {};
    runtime->get_technique_effect_name(technique, eff);
    runtime->get_technique_name(technique, tn);
    entry.effect_name = eff;
    entry.technique_name = tn;

    record(history, std::move(entry));
    return false;
}

void on_preset_changed(reshade::api::effect_runtime *, const char *path)
{
    History &history = state();

    std::string new_path = path ? path : "";
    if (new_path != history.last_preset_path) {
        history.entries.clear();
        history.position = 0;
        save_history_to_file();
    }
    history.last_preset_path = new_path;
}

void on_overlay_frame(reshade::api::effect_runtime *runtime)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput || s_capturing != -1)
        return;

    if (is_pressed(Action::Undo))
        undo(runtime);
    else if (is_pressed(Action::Redo))
        redo(runtime);
    else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, true))
        redo(runtime);
}

void on_runtime_init(reshade::api::effect_runtime *runtime)
{
    init_keybinds(runtime);
    reshade::set_config_value(runtime, "INPUT", "InputProcessing", 1);
}

// --- overlay ui --------------------------------------------------------------

void on_draw_overlay(reshade::api::effect_runtime *runtime)
{
    try {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Effect History (undo/redo)");
        ImGui::PopTextWrapPos();

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Click an entry to jump.\n%zu undoable, %zu redoable.",
                           undo_count(), redo_count());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // quick undo / redo action buttons
        ImGui::BeginDisabled(undo_count() == 0);
        char undo_btn[64];
        char u_desc[32];
        describe_binding(Action::Undo, u_desc, sizeof(u_desc));
        snprintf(undo_btn, sizeof(undo_btn), "Undo (%s)", u_desc);
        if (ImGui::Button(undo_btn, ImVec2(140.0f, 0))) {
            undo(runtime);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(redo_count() == 0);
        char redo_btn[64];
        char r_desc[32];
        describe_binding(Action::Redo, r_desc, sizeof(r_desc));
        snprintf(redo_btn, sizeof(redo_btn), "Redo (%s)", r_desc);
        if (ImGui::Button(redo_btn, ImVec2(140.0f, 0))) {
            redo(runtime);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();

        size_t hsize = history_size();
        if (hsize == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped("No edits recorded yet.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else {
            ImGui::BeginChild("EffectHistory", ImVec2(0, 180), true, ImGuiWindowFlags_HorizontalScrollbar);
            bool is_at_base = (undo_count() == 0);
            if (!is_at_base)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            if (ImGui::Selectable("[Initial Preset State]", is_at_base)) {
                jump_to_position(runtime, hsize);
            }
            if (!is_at_base)
                ImGui::PopStyleColor();

            for (size_t i = hsize; i-- > 0;) {
                char label[256] = {};
                if (!history_label(i, label, sizeof(label)))
                    continue;
                bool undone = history_undone(i);
                bool is_selected = (!undone && (i == redo_count()));
                if (undone)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label, is_selected)) {
                    jump_to_position(runtime, i);
                }
                ImGui::PopID();
                if (undone)
                    ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }

        if (ImGui::Button("Clear History")) {
            clear_history();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // keybindings remapping section
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Keybindings");
        ImGui::PopTextWrapPos();

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Click a binding, then press the desired key combination. Modifiers (Ctrl/Shift/Alt) are recorded automatically.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        struct HeldState {
            bool ctrl = false;
            bool shift = false;
            bool alt = false;
            ImGuiKey normal_key = ImGuiKey_None;
            ImGuiKey last_mod_key = ImGuiKey_None;

            bool empty() const {
                return !ctrl && !shift && !alt && (normal_key == ImGuiKey_None);
            }
            int count() const {
                return (ctrl ? 1 : 0) + (shift ? 1 : 0) + (alt ? 1 : 0) + (normal_key != ImGuiKey_None ? 1 : 0);
            }
        };

        static int s_capturing_start_frame = 0;
        static HeldState s_peak_chord;
        static bool s_has_peak = false;
        static uint64_t s_release_start_ms = 0;
        static bool s_in_release = false;
        static bool s_prev_ctrl = false;
        static bool s_prev_shift = false;
        static bool s_prev_alt = false;
        static ImGuiKey s_last_mod_down = ImGuiKey_None;

        constexpr int kActionCount = static_cast<int>(Action::Count);
        for (int a = 0; a < kActionCount; ++a) {
            Action act = static_cast<Action>(a);
            ImGui::Text("%s", action_name(act));
            ImGui::SameLine(120.0f);

            if (s_capturing == a) {
                // cancel on escape or click away
                if (ImGui::GetFrameCount() > s_capturing_start_frame + 2) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        s_capturing = -1;
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) && !s_has_peak) {
                        s_capturing = -1;
                    }
                }

                if (s_capturing == a) {
                    HeldState current;
                    current.ctrl  = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) || ImGui::GetIO().KeyCtrl;
                    current.shift = ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0) || ImGui::GetIO().KeyShift;
                    current.alt   = ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0) || ImGui::GetIO().KeyAlt;

                    if (current.ctrl && !s_prev_ctrl)   s_last_mod_down = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
                    if (current.shift && !s_prev_shift) s_last_mod_down = (GetAsyncKeyState(VK_RSHIFT)   & 0x8000) ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
                    if (current.alt && !s_prev_alt)     s_last_mod_down = (GetAsyncKeyState(VK_RMENU)    & 0x8000) ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;

                    s_prev_ctrl  = current.ctrl;
                    s_prev_shift = current.shift;
                    s_prev_alt   = current.alt;

                    for (int kk = static_cast<int>(ImGuiKey_NamedKey_BEGIN); kk < static_cast<int>(ImGuiKey_NamedKey_END); ++kk) {
                        ImGuiKey k = static_cast<ImGuiKey>(kk);
                        if (k == ImGuiKey_Escape) continue;
                        if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseMiddle) continue;
                        if (is_modifier_key(k)) continue;
                        if (k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper) continue;

                        if (ImGui::IsKeyDown(k)) {
                            current.normal_key = k;
                            break;
                        }
                    }
                    current.last_mod_key = s_last_mod_down;

                    uint64_t now = GetTickCount64();
                    constexpr uint64_t kReleaseBufferMs = 250;
                    int cur_count = current.count();
                    int peak_count = s_has_peak ? s_peak_chord.count() : 0;

                    if (cur_count > 0) {
                        if (cur_count >= peak_count || (current.normal_key != ImGuiKey_None && s_peak_chord.normal_key == ImGuiKey_None)) {
                            s_peak_chord = current;
                            s_has_peak = true;
                            s_in_release = false;
                            s_release_start_ms = 0;
                        } else {
                            if (!s_in_release) {
                                s_in_release = true;
                                s_release_start_ms = now;
                            } else if (now - s_release_start_ms > kReleaseBufferMs) {
                                s_peak_chord = current;
                                s_in_release = false;
                                s_release_start_ms = 0;
                            }
                        }
                    }

                    int anim_dots = 1 + static_cast<int>(fmod(ImGui::GetTime() * 3.0, 3.0));
                    std::string dots_str(anim_dots, '.');
                    std::string preview;

                    if (!current.empty()) {
                        if (current.ctrl)  preview += "Ctrl+";
                        if (current.shift) preview += "Shift+";
                        if (current.alt)   preview += "Alt+";
                        if (current.normal_key != ImGuiKey_None) {
                            preview += ImGui::GetKeyName(current.normal_key);
                        } else {
                            preview += dots_str;
                        }
                    } else if (s_has_peak && !s_peak_chord.empty()) {
                        if (s_peak_chord.ctrl)  preview += "Ctrl+";
                        if (s_peak_chord.shift) preview += "Shift+";
                        if (s_peak_chord.alt)   preview += "Alt+";
                        if (s_peak_chord.normal_key != ImGuiKey_None) {
                            preview += ImGui::GetKeyName(s_peak_chord.normal_key);
                        } else if (s_peak_chord.last_mod_key != ImGuiKey_None) {
                            preview += (is_shift_key(s_peak_chord.last_mod_key) ? "Shift" :
                                        is_ctrl_key(s_peak_chord.last_mod_key) ? "Ctrl" : "Alt");
                        }
                    } else {
                        preview = "Press a key" + dots_str;
                    }

                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", preview.c_str());

                    // commit once all keys released
                    if (s_has_peak && current.empty() && ImGui::GetFrameCount() > s_capturing_start_frame + 2) {
                        ImGuiKey primary_key = ImGuiKey_None;
                        bool req_ctrl = false;
                        bool req_shift = false;
                        bool req_alt = false;

                        if (s_peak_chord.normal_key != ImGuiKey_None) {
                            primary_key = s_peak_chord.normal_key;
                            req_ctrl  = s_peak_chord.ctrl;
                            req_shift = s_peak_chord.shift;
                            req_alt   = s_peak_chord.alt;
                        } else if (s_peak_chord.count() > 0) {
                            ImGuiKey mod = s_peak_chord.last_mod_key;
                            if (mod == ImGuiKey_None) {
                                if (s_peak_chord.shift) mod = ImGuiKey_LeftShift;
                                else if (s_peak_chord.ctrl) mod = ImGuiKey_LeftCtrl;
                                else if (s_peak_chord.alt) mod = ImGuiKey_LeftAlt;
                            }
                            primary_key = mod;
                            if (is_ctrl_key(mod)) {
                                req_shift = s_peak_chord.shift;
                                req_alt   = s_peak_chord.alt;
                            } else if (is_shift_key(mod)) {
                                req_ctrl = s_peak_chord.ctrl;
                                req_alt  = s_peak_chord.alt;
                            } else if (is_alt_key(mod)) {
                                req_ctrl  = s_peak_chord.ctrl;
                                req_shift = s_peak_chord.shift;
                            }
                        }

                        if (primary_key != ImGuiKey_None) {
                            Binding &b = g_bindings[a];
                            b.key = primary_key;
                            b.ctrl = req_ctrl;
                            b.shift = req_shift;
                            b.alt = req_alt;
                            save_binding(act);
                        }

                        s_capturing = -1;
                        s_peak_chord = HeldState{};
                        s_has_peak = false;
                        s_in_release = false;
                        s_release_start_ms = 0;
                        s_prev_ctrl = false;
                        s_prev_shift = false;
                        s_prev_alt = false;
                        s_last_mod_down = ImGuiKey_None;
                    }
                }
            } else {
                char b[96];
                describe_binding(act, b, sizeof(b));
                char id[104];
                snprintf(id, sizeof(id), "%s###kb%d", b, a);
                if (ImGui::Button(id, ImVec2(120.0f, 0))) {
                    s_capturing = a;
                    s_capturing_start_frame = ImGui::GetFrameCount();
                    s_peak_chord = HeldState{};
                    s_has_peak = false;
                    s_in_release = false;
                    s_release_start_ms = 0;
                    s_prev_ctrl = false;
                    s_prev_shift = false;
                    s_prev_alt = false;
                    s_last_mod_down = ImGuiKey_None;
                }
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Reset Keybinds to Defaults")) {
            reset_keybinds();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Undo / Redo v1.0.0  -  by NotRayST");
        ImGui::PopTextWrapPos();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        if (ImGui::Button("GitHub")) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab", nullptr, nullptr, SW_SHOW);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("https://github.com/NotRayST/ShaderLab");
        }

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        if (ImGui::Button("Support me on Patreon")) {
            ShellExecuteW(nullptr, L"open", L"https://www.patreon.com/cw/RayST", nullptr, nullptr, SW_SHOW);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("https://www.patreon.com/cw/RayST");
        }

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(1);
        ImGui::PopStyleVar(2);
    } catch (...) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error rendering Undo/Redo overlay");
        ImGui::PopTextWrapPos();
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;

        init_keybinds(nullptr);
        load_history_from_file();

        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);
        reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
        reshade::register_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
        reshade::register_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
        reshade::register_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
        reshade::register_overlay(nullptr, &on_draw_overlay);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(nullptr, &on_draw_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
        reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
        reshade::unregister_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
        reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);

        delete_history_file();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
