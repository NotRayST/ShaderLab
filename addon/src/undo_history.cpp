#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "undo_history.h"
#include "keybinds.h"
#include "job_queue.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <windows.h>

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
        return L"ShaderLab_undo_history.dat";
    return std::wstring(temp) + L"ShaderLab_undo_history.dat";
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

void save_history()
{
    save_history_to_file();
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

static void mark_project_dirty()
{
    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (block && queue_mgr.is_connected() && block->active_project_path[0] != L'\0') {
        block->project_dirty = 1;
    }
}

void record(History &history, HistoryEntry entry)
{
    while (history.position > 0) {
        history.entries.pop_front();
        --history.position;
    }

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
    save_history();
    mark_project_dirty();
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
    save_history();
    mark_project_dirty();
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
    save_history();
    mark_project_dirty();
}

void jump_to_position(reshade::api::effect_runtime *runtime, size_t target_position)
{
    History &history = state();
    target_position = std::min(target_position, history.entries.size());

    while (history.position < target_position)
        undo(runtime);
    while (history.position > target_position)
        redo(runtime);
}

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
                std::min(new_value_size, sizeof(HistoryEntry::Value)));

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

void on_overlay_frame(reshade::api::effect_runtime *runtime)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput)
        return;

    if (keybinds::is_pressed(keybinds::Action::Undo))
        undo(runtime);
    else if (keybinds::is_pressed(keybinds::Action::Redo))
        redo(runtime);
    else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, true))
        redo(runtime);
}

void on_preset_changed(reshade::api::effect_runtime *, const char *path)
{
    History &history = state();

    std::string new_path = path ? path : "";
    if (new_path != history.last_preset_path) {
        history.entries.clear();
        history.position = 0;
        save_history();
        mark_project_dirty();
    }
    history.last_preset_path = new_path;
}

void on_runtime_init(reshade::api::effect_runtime *runtime)
{
    keybinds::init(runtime);
    reshade::set_config_value(runtime, "INPUT", "InputProcessing", 1);
}

} // namespace

namespace undo_history {

void init()
{
    keybinds::init(nullptr);
    load_history_from_file();

    reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
    reshade::register_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
    reshade::register_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
    reshade::register_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
}

void shutdown()
{
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_overlay_frame);
    reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(&on_set_technique_state);
    reshade::unregister_event<reshade::addon_event::reshade_set_uniform_value>(&on_set_uniform_value);
    reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(&on_preset_changed);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_runtime_init);
    keybinds::shutdown();

    delete_history_file();
}

size_t undo_count() { return state().entries.size() - state().position; }
size_t redo_count() { return state().position; }
size_t history_size() { return state().entries.size(); }

bool history_undone(size_t index)
{
    return index < state().position;
}

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

void jump_to(reshade::api::effect_runtime *runtime, size_t index)
{
    jump_to_position(runtime, index);
}

void clear()
{
    History &history = state();
    history.entries.clear();
    history.position = 0;
    save_history();
}

} // namespace undo_history
