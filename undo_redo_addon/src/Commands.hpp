#pragma once
#define ImTextureID ImU64
#include <imgui.h>
#ifndef GImGui
#define GImGui (ImGui::GetCurrentContext())
#endif
#include <imgui_internal.h>
#include <reshade.hpp>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include "ICommand.hpp"
#include "DynamicPayload.hpp"
#include "AddonRegistry.hpp"

extern reshade::api::effect_runtime *g_runtime;

inline reshade::api::effect_uniform_variable resolve_uniform(
    reshade::api::effect_runtime *rt, const std::string &eff, const std::string &var)
{
    if (!rt) rt = g_runtime;
    if (!rt) return { 0 };
    auto u = rt->find_uniform_variable(eff.c_str(), var.c_str());
    return u != reshade::api::effect_uniform_variable{ 0 } ? u : rt->find_uniform_variable(nullptr, var.c_str());
}

inline reshade::api::effect_technique resolve_technique(
    reshade::api::effect_runtime *rt, const std::string &eff, const std::string &tech)
{
    if (!rt) rt = g_runtime;
    if (!rt) return { 0 };
    auto t = rt->find_technique(eff.c_str(), tech.c_str());
    return t != reshade::api::effect_technique{ 0 } ? t : rt->find_technique(nullptr, tech.c_str());
}

inline void apply_uniform_payload(
    reshade::api::effect_runtime *rt, const std::string &eff,
    const std::string &var, const DynamicPayload &p)
{
    if (!rt) rt = g_runtime;
    if (!rt) return;
    auto v = resolve_uniform(rt, eff, var);
    if (v == reshade::api::effect_uniform_variable{ 0 }) return;

    size_t esz = p.format == reshade::api::format::r32_typeless ? sizeof(bool) : 4;
    size_t cnt = p.element_count ? p.element_count : (p.bytes.size() / esz);
    if (!cnt || p.bytes.size() < cnt * esz) return;

    switch (p.format) {
    case reshade::api::format::r32_typeless:
        rt->set_uniform_value_bool(v, reinterpret_cast<const bool *>(p.bytes.data()), cnt); break;
    case reshade::api::format::r32_float:
        rt->set_uniform_value_float(v, reinterpret_cast<const float *>(p.bytes.data()), cnt); break;
    case reshade::api::format::r32_sint:
        rt->set_uniform_value_int(v, reinterpret_cast<const int32_t *>(p.bytes.data()), cnt); break;
    case reshade::api::format::r32_uint:
        rt->set_uniform_value_uint(v, reinterpret_cast<const uint32_t *>(p.bytes.data()), cnt); break;
    default: break;
    }
}

template <class T>
inline T as(const DynamicPayload &p) {
    T v{};
    if (p.bytes.size() >= sizeof(T)) std::memcpy(&v, p.bytes.data(), sizeof(T));
    return v;
}

// helper for scalar preview formatting
inline std::string format_scalar(const DynamicPayload &p, const char *scope) {
    char buf[256] = {};
    if (p.element_count > 1 || p.bytes.size() > 4) {
        uint32_t n = p.element_count > 0 ? p.element_count : static_cast<uint32_t>(p.bytes.size() / 4);
        snprintf(buf, sizeof(buf), "Uniform %s [%u elements]", scope, n);
        return buf;
    }
    switch (p.format) {
    case reshade::api::format::r32_typeless: {
        bool v = !p.bytes.empty() && p.bytes[0] != 0;
        snprintf(buf, sizeof(buf), "Uniform %s = %s", scope, v ? "true" : "false"); break; }
    case reshade::api::format::r32_float:
        snprintf(buf, sizeof(buf), "Uniform %s = %.2f", scope, as<float>(p)); break;
    case reshade::api::format::r32_sint:
        snprintf(buf, sizeof(buf), "Uniform %s = %d", scope, as<int32_t>(p)); break;
    case reshade::api::format::r32_uint:
        snprintf(buf, sizeof(buf), "Uniform %s = %u", scope, as<uint32_t>(p)); break;
    default: snprintf(buf, sizeof(buf), "Uniform %s", scope); break;
    }
    return buf;
}

inline std::string format_delta(const DynamicPayload &before, const DynamicPayload &after) {
    char buf[128] = {};
    if (after.element_count > 1 || after.bytes.size() > 4) {
        snprintf(buf, sizeof(buf), "[%u vals]",
                 after.element_count > 0 ? after.element_count : static_cast<uint32_t>(after.bytes.size() / 4));
        return buf;
    }
    switch (after.format) {
    case reshade::api::format::r32_float: {
        float b = as<float>(before), a = as<float>(after);
        snprintf(buf, sizeof(buf), "%.2f -> %.2f (%+.2f)", b, a, a - b); break; }
    case reshade::api::format::r32_sint: {
        int32_t b = as<int32_t>(before), a = as<int32_t>(after);
        snprintf(buf, sizeof(buf), "%d -> %d (%+d)", b, a, a - b); break; }
    case reshade::api::format::r32_uint:
        snprintf(buf, sizeof(buf), "%u -> %u", as<uint32_t>(before), as<uint32_t>(after)); break;
    case reshade::api::format::r32_typeless: {
        bool b = !before.bytes.empty() && before.bytes[0] != 0;
        bool a = !after.bytes.empty() && after.bytes[0] != 0;
        snprintf(buf, sizeof(buf), "%s -> %s", b ? "true" : "false", a ? "true" : "false"); break; }
    default: break;
    }
    return buf;
}

inline bool is_ui_state_variable(const std::string &name) {
    if (name.empty()) return false;
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
    auto has = [&](const char *t) { return s.find(t) != std::string::npos; };
    return has("ui_open") || has("uiopen") ||
           (has("overlay") && (has("open") || has("active") || has("hover"))) ||
           (has("menu") && (has("open") || has("active")));
}

// fnv1a for merge id
inline int32_t fnv_merge_id(const std::string &a, const std::string &b) {
    uint32_t h = 2166136261u;
    for (char c : a) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    h = (h ^ 0x3Au) * 16777619u;
    for (char c : b) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return static_cast<int32_t>(h & 0x7FFFFFFF);
}

class UniformCommand : public ICommand {
public:
    UniformCommand() = default;
    UniformCommand(std::string eff, std::string var, DynamicPayload before, DynamicPayload after)
        : eff_(std::move(eff)), var_(std::move(var)), before_(std::move(before)),
          after_(std::move(after)), ts_(GetTickCount64()) {}

    void execute(reshade::api::effect_runtime *rt = nullptr) override {
        if (is_ui_state_variable(var_)) return;
        if (!rt) rt = g_runtime;
        apply_uniform_payload(rt, eff_, var_, after_);
    }
    void undo(reshade::api::effect_runtime *rt = nullptr) override {
        if (is_ui_state_variable(var_)) return;
        if (!rt) rt = g_runtime;
        apply_uniform_payload(rt, eff_, var_, before_);
    }

    CommandDomain domain() const override { return CommandDomain::ShaderState; }
    uint32_t type_id() const override { return kCommandTypeUniform; }

    std::string get_label() const override {
        const char *scope = var_.empty() ? eff_.c_str() : var_.c_str();
        return format_scalar(after_, scope);
    }
    std::string get_delta_preview() const override { return format_delta(before_, after_); }

    int32_t merge_id() const override { return fnv_merge_id(eff_, var_); }

    bool merge_with(const ICommand *newer) override {
        if (!newer || newer->type_id() != kCommandTypeUniform || GetTickCount64() - ts_ >= 800)
            return false;
        const auto *o = static_cast<const UniformCommand *>(newer);
        if (eff_ != o->eff_ || var_ != o->var_) return false;
        after_ = o->after_;
        ts_ = GetTickCount64();
        return true;
    }

    const std::string &effect_name() const { return eff_; }
    const std::string &variable_name() const { return var_; }
    const DynamicPayload &before_state() const { return before_; }
    const DynamicPayload &after_state() const { return after_; }

private:
    std::string eff_, var_;
    DynamicPayload before_, after_;
    uint64_t ts_ = 0;
};

struct StagedMutation {
    std::string effect_name, variable_name;
    DynamicPayload initial_state, latest_state;
};

class CompoundUniformCommand : public ICommand {
public:
    CompoundUniformCommand() = default;
    explicit CompoundUniformCommand(std::vector<StagedMutation> m) : muts_(std::move(m)) {}

    void execute(reshade::api::effect_runtime *rt = nullptr) override {
        if (!rt) rt = g_runtime;
        for (const auto &m : muts_) apply_uniform_payload(rt, m.effect_name, m.variable_name, m.latest_state);
    }
    void undo(reshade::api::effect_runtime *rt = nullptr) override {
        if (!rt) rt = g_runtime;
        for (auto it = muts_.rbegin(); it != muts_.rend(); ++it)
            apply_uniform_payload(rt, it->effect_name, it->variable_name, it->initial_state);
    }

    CommandDomain domain() const override { return CommandDomain::ShaderState; }
    uint32_t type_id() const override { return kCommandTypeCompoundUniform; }

    std::string get_label() const override {
        if (muts_.empty()) return "Multi-Uniform Edit (empty)";
        char buf[256]; snprintf(buf, sizeof(buf), "Gizmo Transform (%zu uniforms, e.g. %s)",
                                muts_.size(), muts_[0].variable_name.c_str());
        return buf;
    }
    std::string get_delta_preview() const override {
        char buf[64]; snprintf(buf, sizeof(buf), "%zu parameters", muts_.size()); return buf;
    }

    int32_t merge_id() const override {
        if (muts_.empty()) return -1;
        uint32_t h = 2166136261u;
        for (const auto &m : muts_) {
            for (char c : m.effect_name) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
            for (char c : m.variable_name) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
        }
        return static_cast<int32_t>(h & 0x7FFFFFFF);
    }

    bool merge_with(const ICommand *newer) override {
        if (!newer || newer->type_id() != kCommandTypeCompoundUniform || GetTickCount64() - ts_ >= 800)
            return false;
        const auto *o = static_cast<const CompoundUniformCommand *>(newer);
        if (muts_.size() != o->muts_.size()) return false;
        for (size_t i = 0; i < muts_.size(); ++i) {
            if (muts_[i].effect_name != o->muts_[i].effect_name ||
                muts_[i].variable_name != o->muts_[i].variable_name)
                return false;
        }
        for (size_t i = 0; i < muts_.size(); ++i) {
            muts_[i].latest_state = o->muts_[i].latest_state;
        }
        ts_ = GetTickCount64();
        return true;
    }

    const std::vector<StagedMutation> &mutations() const { return muts_; }

private:
    std::vector<StagedMutation> muts_;
    uint64_t ts_ = GetTickCount64();
};

class TechniqueToggleCommand : public ICommand {
public:
    TechniqueToggleCommand() = default;
    TechniqueToggleCommand(std::string eff, std::string tech, bool before, bool after)
        : eff_(std::move(eff)), tech_(std::move(tech)), before_(before), after_(after) {}

    void execute(reshade::api::effect_runtime *rt = nullptr) override {
        if (!rt) rt = g_runtime;
        if (!rt) return;
        auto t = resolve_technique(rt, eff_, tech_);
        if (t != reshade::api::effect_technique{ 0 }) rt->set_technique_state(t, after_);
    }
    void undo(reshade::api::effect_runtime *rt = nullptr) override {
        if (!rt) rt = g_runtime;
        if (!rt) return;
        auto t = resolve_technique(rt, eff_, tech_);
        if (t != reshade::api::effect_technique{ 0 }) rt->set_technique_state(t, before_);
    }

    CommandDomain domain() const override { return CommandDomain::ShaderState; }
    uint32_t type_id() const override { return kCommandTypeTechniqueToggle; }
    std::string get_label() const override { return tech_ + (after_ ? " on" : " off"); }
    std::string get_delta_preview() const override {
        return std::string(before_ ? "[ON]" : "[OFF]") + " -> " + (after_ ? "[ON]" : "[OFF]");
    }

    const std::string &technique_name() const { return tech_; }
    bool before_state() const { return before_; }
    bool after_state() const { return after_; }

private:
    std::string eff_, tech_;
    bool before_ = false, after_ = false;
};



class ExternalAddonCommand : public ICommand {
public:
    ExternalAddonCommand() = default;
    ExternalAddonCommand(std::string id, std::string tag, std::string label,
                         std::vector<uint8_t> before, std::vector<uint8_t> after)
        : id_(std::move(id)), tag_(std::move(tag)), label_(std::move(label)),
          before_(std::move(before)), after_(std::move(after)) {}

    void execute(reshade::api::effect_runtime * = nullptr) override {
        AddonRegistry::instance().dispatch_action(id_, tag_, after_, false);
    }
    void undo(reshade::api::effect_runtime * = nullptr) override {
        AddonRegistry::instance().dispatch_action(id_, tag_, before_, true);
    }

    CommandDomain domain() const override { return CommandDomain::AddonCustom; }
    uint32_t type_id() const override { return kCommandTypeExternalAddon; }
    std::string get_label() const override { return label_.empty() ? "[" + id_ + "] " + tag_ : label_; }
    std::string get_delta_preview() const override { return label_.empty() ? tag_ : label_; }

private:
    std::string id_, tag_, label_;
    std::vector<uint8_t> before_, after_;
};
