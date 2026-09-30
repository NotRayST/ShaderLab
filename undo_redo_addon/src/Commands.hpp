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

inline bool is_reshade_main_dock_node(const ImGuiDockNode *node) {
    if (!node) return false;
    if (node->IsFloatingNode() && node->Windows.Size <= 1) return false;
    for (int i = 0; i < node->Windows.Size; ++i) {
        const ImGuiWindow *w = node->Windows[i];
        if (w && w->Name &&
            (strstr(w->Name, "###home") || strstr(w->Name, "###settings") ||
             strstr(w->Name, "###addons") || strstr(w->Name, "###statistics") ||
             strstr(w->Name, "###log") || strstr(w->Name, "###about")))
            return true;
    }
    return (node->ChildNodes[0] && is_reshade_main_dock_node(node->ChildNodes[0])) ||
           (node->ChildNodes[1] && is_reshade_main_dock_node(node->ChildNodes[1]));
}

class WindowTransformCommand : public ICommand {
public:
    WindowTransformCommand() = default;
    WindowTransformCommand(std::string name,
                           const float bp[2], const float bs[2],
                           const float ap[2], const float as[2],
                           int bt = -1, int at = -1, uint32_t bd = 0, uint32_t ad = 0)
        : name_(std::move(name)), bt_(bt), at_(at), bd_(bd), ad_(ad)
    {
        if (bp) std::memcpy(bp_, bp, 8); if (bs) std::memcpy(bs_, bs, 8);
        if (ap) std::memcpy(ap_, ap, 8); if (as) std::memcpy(as_, as, 8);
    }

    static inline bool s_defer_transforms = false;
    struct DeferredTransform {
        std::string name;
        float p[2];
        float s[2];
        uint32_t dock;
    };
    static inline std::unordered_map<std::string, DeferredTransform> s_deferred_transforms;

    static void set_defer_transforms(bool defer) {
        s_defer_transforms = defer;
        if (!defer) {
            flush_deferred_transforms();
        }
    }

    static bool has_deferred_transforms() {
        return !s_deferred_transforms.empty();
    }

    static void flush_deferred_transforms() {
        for (const auto &kv : s_deferred_transforms) {
            apply_raw_geometry(kv.second.name, kv.second.p, kv.second.s, kv.second.dock);
        }
        s_deferred_transforms.clear();
    }

    static void apply_raw_geometry(const std::string &name, const float p[2], const float s[2], uint32_t dock) {
        (void)dock;
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        const ImGuiIO &io = ImGui::GetIO();
        float ww = io.DisplaySize.x > 0 ? io.DisplaySize.x : 1920.0f;
        float wh = io.DisplaySize.y > 0 ? io.DisplaySize.y : 1080.0f;

        float cx = (std::max)(0.0f, (std::min)(p[0], ww - (std::min)(s[0], 120.0f)));
        float cy = (std::max)(0.0f, (std::min)(p[1], wh - 35.0f));
        float cw = (std::min)((std::max)(s[0], 100.0f), ww);
        float ch = (std::min)((std::max)(s[1], 50.0f), wh);

        if (name == "ReShade") {
            for (ImGuiWindow *w : ctx->Windows) {
                if (!w || !w->DockNode || !is_reshade_main_dock_node(w->DockNode)) continue;
                ImGuiDockNode *node = w->DockNode;
                node->Pos = ImVec2(cx, cy); node->Size = ImVec2(cw, ch); node->SizeRef = ImVec2(cw, ch);
                if (node->HostWindow) {
                    node->HostWindow->Pos = ImVec2(cx, cy);
                    node->HostWindow->Size = node->HostWindow->SizeFull = ImVec2(cw, ch);
                    if (node->HostWindow->Name) {
                        ImGui::SetWindowPos(node->HostWindow->Name, ImVec2(cx, cy), ImGuiCond_Always);
                        ImGui::SetWindowSize(node->HostWindow->Name, ImVec2(cw, ch), ImGuiCond_Always);
                    }
                }
                for (int i = 0; i < node->Windows.Size; ++i) {
                    if (ImGuiWindow *dw = node->Windows[i]) {
                        dw->Pos = ImVec2(cx, cy); dw->Size = dw->SizeFull = ImVec2(cw, ch);
                        if (dw->Name) {
                            ImGui::SetWindowPos(dw->Name, ImVec2(cx, cy), ImGuiCond_Always);
                            ImGui::SetWindowSize(dw->Name, ImVec2(cw, ch), ImGuiCond_Always);
                        }
                    }
                }
                break;
            }
            return;
        }

        ImGui::SetWindowPos(name.c_str(), ImVec2(cx, cy), ImGuiCond_Always);
        ImGui::SetWindowSize(name.c_str(), ImVec2(cw, ch), ImGuiCond_Always);

        for (ImGuiWindow *w : ctx->Windows) {
            if (!w || !w->Name || strcmp(w->Name, name.c_str()) != 0) continue;
            w->Pos = ImVec2(cx, cy); w->Size = w->SizeFull = ImVec2(cw, ch);

            if (w->DockNode && w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size == 1) {
                w->DockNode->Pos = ImVec2(cx, cy);
                w->DockNode->Size = w->DockNode->SizeRef = ImVec2(cw, ch);
            }
            break;
        }
    }

    void apply_geometry(bool is_undo) {
        const float *p = is_undo ? bp_ : ap_;
        const float *s = is_undo ? bs_ : as_;
        uint32_t dock = is_undo ? bd_ : ad_;

        if (s_defer_transforms) {
            s_deferred_transforms[name_] = { name_, { p[0], p[1] }, { s[0], s[1] }, dock };
            return;
        }

        apply_raw_geometry(name_, p, s, dock);
    }

    void execute(reshade::api::effect_runtime * = nullptr) override { apply_geometry(false); }
    void undo(reshade::api::effect_runtime * = nullptr) override { apply_geometry(true); }

    CommandDomain domain() const override { return CommandDomain::WindowLayout; }
    uint32_t type_id() const override { return kCommandTypeWindowTransform; }

    static bool differs(const float a[2], const float b[2]) {
        return fabs(a[0] - b[0]) >= 1.0f || fabs(a[1] - b[1]) >= 1.0f;
    }

    std::string get_label() const override {
        std::string dn = name_.substr(0, name_.find("###"));
        char buf[256];
        if (differs(bs_, as_)) snprintf(buf, sizeof(buf), "Resize '%s' (%.0fx%.0f)", dn.c_str(), as_[0], as_[1]);
        else                   snprintf(buf, sizeof(buf), "Move '%s' (%.0f, %.0f)", dn.c_str(), ap_[0], ap_[1]);
        return buf;
    }

    std::string get_delta_preview() const override {
        char buf[128];
        if (differs(bs_, as_)) snprintf(buf, sizeof(buf), "(%.0fx%.0f) -> (%.0fx%.0f)", bs_[0], bs_[1], as_[0], as_[1]);
        else                   snprintf(buf, sizeof(buf), "(%.0f, %.0f) -> (%.0f, %.0f)", bp_[0], bp_[1], ap_[0], ap_[1]);
        return buf;
    }

private:
    std::string name_;
    float bp_[2] = {}, bs_[2] = {}, ap_[2] = {}, as_[2] = {};
    int bt_ = -1, at_ = -1;
    uint32_t bd_ = 0, ad_ = 0;
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
