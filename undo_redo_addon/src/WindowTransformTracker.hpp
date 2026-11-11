#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#define ImTextureID ImU64
#include <imgui.h>
#ifndef GImGui
#define GImGui (ImGui::GetCurrentContext())
#endif
#include <imgui_internal.h>
#include <reshade.hpp>
#include "ICommand.hpp"
#include "ContextManager.hpp"

#include "TabDockingTracker.hpp"

namespace {

inline bool subtree_has_builtin_tab(const ImGuiDockNode *n) {
    if (!n) return false;
    for (ImGuiWindow *w : n->Windows)
        if (is_builtin_tab(w)) return true;
    return subtree_has_builtin_tab(n->ChildNodes[0]) || subtree_has_builtin_tab(n->ChildNodes[1]);
}

inline std::string describe_dock_subtree(const ImGuiDockNode *n) {
    if (!n) return "Panel";
    if (subtree_has_builtin_tab(n)) return "ReShade";
    if (n->VisibleWindow && n->VisibleWindow->Name) return clean_dock_tab_name(n->VisibleWindow->Name);
    for (ImGuiWindow *w : n->Windows)
        if (w && w->Name) return clean_dock_tab_name(w->Name);
    if (n->ChildNodes[0] && !n->ChildNodes[0]->IsCentralNode()) return describe_dock_subtree(n->ChildNodes[0]);
    if (n->ChildNodes[1] && !n->ChildNodes[1]->IsCentralNode()) return describe_dock_subtree(n->ChildNodes[1]);
    return "Panel";
}

inline void collect_visible_split_nodes(ImGuiDockNode *n, std::vector<ImGuiDockNode *> &out) {
    if (!n || !n->IsSplitNode()) return;
    ImGuiDockNode *c0 = n->ChildNodes[0], *c1 = n->ChildNodes[1];
    if (c0 && c1 && c0->IsVisible && c1->IsVisible &&
        (n->SplitAxis == ImGuiAxis_X || n->SplitAxis == ImGuiAxis_Y)) {
        out.push_back(n);
    }
    if (c0) collect_visible_split_nodes(c0, out);
    if (c1) collect_visible_split_nodes(c1, out);
}

#ifndef UNDOREDO_DOCKSPACE_HELPERS_DEFINED
#define UNDOREDO_DOCKSPACE_HELPERS_DEFINED
inline ImGuiDockNode *find_viewport_dockspace_root(ImGuiContext *ctx) {
    if (!ctx) return nullptr;
    for (ImGuiWindow *w : ctx->Windows) {
        if (!w) continue;
        if (w->DockNodeAsHost && w->DockNodeAsHost->IsDockSpace())
            return w->DockNodeAsHost;
        if (w->DockNode) {
            ImGuiDockNode *r = w->DockNode;
            while (r->ParentNode) r = r->ParentNode;
            if (r->IsDockSpace()) return r;
        }
    }
    return nullptr;
}

inline ImGuiDockNode *find_node_in_tree(ImGuiDockNode *n, ImGuiID id) {
    if (!n || id == 0) return nullptr;
    if (n->ID == id) return n;
    if (ImGuiDockNode *c0 = find_node_in_tree(n->ChildNodes[0], id)) return c0;
    return find_node_in_tree(n->ChildNodes[1], id);
}
#endif

inline void clamp_window_rect(const ImGuiIO &io, ImVec2 raw_pos, ImVec2 raw_sz, ImVec2 &out_pos, ImVec2 &out_sz) {
    float ww = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1920.0f;
    float wh = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 1080.0f;
    out_sz = ImVec2(std::clamp(raw_sz.x, 100.0f, ww), std::clamp(raw_sz.y, 50.0f, wh));
    out_pos = ImVec2(std::clamp(raw_pos.x, 0.0f, ww - std::min(out_sz.x, 120.0f)),
                     std::clamp(raw_pos.y, 0.0f, wh - 35.0f));
}

} // namespace

class WindowTransformTracker {
public:
    struct PendingWindowApply {
        std::string name;
        ImVec2 pos = {}, size = {};
        bool is_container = false;
        bool apply_pos = true;
        bool apply_size = true;
    };

    struct PendingSplitApply {
        ImGuiID split_id = 0, c0_id = 0, c1_id = 0;
        ImGuiAxis axis = ImGuiAxis_X;
        float s0 = 0.0f, s1 = 0.0f;
    };

    struct FloatState {
        std::string name;
        bool is_container = false;
        bool was_floating = false;
        bool has_baseline = false;
        ImGuiID dock_id = 0;
        ImVec2 pos = {}, size = {};
    };

    struct SplitState {
        ImGuiID c0_id = 0, c1_id = 0;
        ImGuiAxis axis = ImGuiAxis_X;
        float parent_size = 0.0f;
        float c0_size = 0.0f, c1_size = 0.0f;
        bool has_baseline = false;
    };

    static void set_defer(bool defer) {
        auto &tr = instance();
        tr.defer_ = defer;
        if (defer)
            tr.suppress_ = (std::max)(tr.suppress_, 4);
    }

    static bool is_defer() {
        return instance().defer_;
    }

    static bool has_deferred() {
        auto &tr = instance();
        return !tr.pending_win_.empty() || !tr.pending_split_.empty();
    }

    static void flush_deferred(ImGuiContext *ctx) {
        if (!ctx) return;
        auto &tr = instance();
        tr.defer_ = false;
        if (tr.pending_win_.empty() && tr.pending_split_.empty()) return;
        tr.suppress_ = g_suppress_tracking_frames = 6;
        if (!tr.pending_win_.empty()) {
            std::vector<PendingWindowApply> batch;
            batch.swap(tr.pending_win_);
            for (const auto &p : batch)
                apply_window_now(ctx, p.name, p.pos, p.size, p.is_container, p.apply_pos, p.apply_size);
        }
        if (!tr.pending_split_.empty()) {
            std::vector<PendingSplitApply> batch;
            batch.swap(tr.pending_split_);
            for (const auto &p : batch)
                apply_split_now(ctx, p.split_id, p.c0_id, p.c1_id, p.axis, p.s0, p.s1);
        }
        sync_baselines(ctx);
    }

    static void apply_window(const std::string &name, ImVec2 pos, ImVec2 size, bool is_container = false,
                             bool apply_pos = true, bool apply_size = true) {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        instance().suppress_ = g_suppress_tracking_frames = 4;

        auto &pend = instance().pending_win_;
        pend.erase(std::remove_if(pend.begin(), pend.end(),
            [&](const PendingWindowApply &p) { return p.name == name; }), pend.end());

        if (instance().defer_ || ctx->CurrentWindowStack.Size > 1) {
            pend.push_back({name, pos, size, is_container, apply_pos, apply_size});
            return;
        }
        apply_window_now(ctx, name, pos, size, is_container, apply_pos, apply_size);
    }

    static void apply_split(ImGuiID split_id, ImGuiID c0_id, ImGuiID c1_id,
                            ImGuiAxis axis, float s0, float s1) {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        instance().suppress_ = g_suppress_tracking_frames = 4;

        auto &pend = instance().pending_split_;
        pend.erase(std::remove_if(pend.begin(), pend.end(),
            [&](const PendingSplitApply &p) { return p.split_id == split_id; }), pend.end());

        if (instance().defer_ || ctx->CurrentWindowStack.Size > 1) {
            pend.push_back({split_id, c0_id, c1_id, axis, s0, s1});
            return;
        }
        apply_split_now(ctx, split_id, c0_id, c1_id, axis, s0, s1);
    }

    static void update(ImGuiContext *ctx);

private:
    static WindowTransformTracker &instance() {
        static WindowTransformTracker s;
        return s;
    }

    static void apply_node_rect(ImGuiDockNode *n, ImVec2 pos, ImVec2 sz) {
        if (!n) return;
        n->Pos = pos;
        n->Size = n->SizeRef = sz;
        if (n->HostWindow) {
            n->HostWindow->SetWindowPosAllowFlags |= ImGuiCond_Always;
            n->HostWindow->SetWindowSizeAllowFlags |= ImGuiCond_Always;
            n->HostWindow->Pos = pos;
            n->HostWindow->Size = n->HostWindow->SizeFull = sz;
            if (n->HostWindow->Name) {
                ImGui::SetWindowPos(n->HostWindow->Name, pos, ImGuiCond_Always);
                ImGui::SetWindowSize(n->HostWindow->Name, sz, ImGuiCond_Always);
            }
        }
    }

    static void apply_window_now(ImGuiContext *ctx, const std::string &name,
                                 ImVec2 raw_pos, ImVec2 raw_size, bool is_container,
                                 bool apply_pos = true, bool apply_size = true) {
        ImVec2 pos, sz;
        clamp_window_rect(ImGui::GetIO(), raw_pos, raw_size, pos, sz);

        if (is_container || name == "ReShade") {
            for (ImGuiWindow *w : ctx->Windows) {
                if (!w || !w->DockNode || !w->DockNode->IsFloatingNode() || w->DockNode->Windows.Size <= 1)
                    continue;
                ImGuiDockNode *n = w->DockNode;
                if (name == "ReShade" ? !subtree_has_builtin_tab(n) : (find_window_by_name(ctx, name) != w))
                    continue;

                ImVec2 target_p = apply_pos ? pos : n->Pos;
                ImVec2 target_s = apply_size ? sz : n->Size;
                apply_node_rect(n, target_p, target_s);

                // apply_node_rect sets the container dock node and its host window rect.
                // Child windows inside the dock node are positioned and sized by ImGui docking.
                auto &st = instance().float_tracked_[n->ID];
                if (apply_pos) st.pos = pos;
                if (apply_size) st.size = sz;
                st.has_baseline = true;
                return;
            }
        }

        ImGuiWindow *w = find_window_by_name(ctx, name);
        if (!w) {
            if (!is_container && name != "ReShade") {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "[UndoRedo] Window transform apply skipped: window '%s' not found.", name.c_str());
                reshade::log::message(reshade::log::level::warning, msg);
            }
            return;
        }

        // Only apply pos/size directly if window is truly floating.
        // Applying raw pos/size to a docked window causes it to fight against its dock node and desync.
        if (is_window_truly_floating(w)) {
            if (apply_pos) {
                w->SetWindowPosAllowFlags |= ImGuiCond_Always;
                w->Pos = pos;
                ImGui::SetWindowPos(w->Name, pos, ImGuiCond_Always);
            }
            if (apply_size) {
                w->SetWindowSizeAllowFlags |= ImGuiCond_Always;
                w->Size = w->SizeFull = sz;
                ImGui::SetWindowSize(w->Name, sz, ImGuiCond_Always);
            }

            if (w->DockNode && w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size <= 1) {
                ImVec2 target_p = apply_pos ? pos : w->DockNode->Pos;
                ImVec2 target_s = apply_size ? sz : w->DockNode->Size;
                apply_node_rect(w->DockNode, target_p, target_s);
            }
        }

        auto &st = instance().float_tracked_[w->ID];
        if (apply_pos) st.pos = pos;
        if (apply_size) st.size = sz;
        st.has_baseline = true;
    }

    static void apply_split_now(ImGuiContext *ctx, ImGuiID split_id, ImGuiID c0_id, ImGuiID c1_id,
                                ImGuiAxis axis, float target_s0, float target_s1) {
        if (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) return;
        ImGuiDockNode *root = find_viewport_dockspace_root(ctx);
        ImGuiDockNode *p = find_node_in_tree(root, split_id);
        if (!p || !p->IsSplitNode()) {
            if (ImGuiDockNode *c0 = find_node_in_tree(root, c0_id)) p = c0->ParentNode;
            else if (ImGuiDockNode *c1 = find_node_in_tree(root, c1_id)) p = c1->ParentNode;
        }
        if (!p || !p->ChildNodes[0] || !p->ChildNodes[1]) {
            reshade::log::message(reshade::log::level::warning, "[UndoRedo] Dock splitter apply skipped: split node not found.");
            return;
        }

        ImGuiDockNode *c0 = p->ChildNodes[0], *c1 = p->ChildNodes[1];
        float spacing = ctx->Style.DockingSeparatorSize;
        float avail = p->Size[axis] > spacing ? (p->Size[axis] - spacing) : (target_s0 + target_s1);
        float s0 = target_s0, s1 = target_s1;
        if (avail > 0.0f && (s0 + s1) > 0.0f && std::fabs((s0 + s1) - avail) > 1.0f) {
            float r = std::clamp(s0 / (s0 + s1), 0.05f, 0.95f);
            s0 = std::floor(avail * r + 0.5f);
            s1 = avail - s0;
        }
        c0->WantLockSizeOnce = c1->WantLockSizeOnce = true;
        c0->SizeRef[axis] = c0->Size[axis] = s0;
        c1->SizeRef[axis] = c1->Size[axis] = s1;
        if (axis == ImGuiAxis_X) {
            c1->Pos.x = c0->Pos.x + s0 + spacing;
        } else if (axis == ImGuiAxis_Y) {
            c1->Pos.y = c0->Pos.y + s0 + spacing;
        }

        auto &st = instance().split_tracked_[p->ID];
        st.c0_id = c0->ID;
        st.c1_id = c1->ID;
        st.axis = axis;
        st.parent_size = p->Size[axis];
        st.c0_size = s0;
        st.c1_size = s1;
        st.has_baseline = true;
    }

    static void sync_baselines(ImGuiContext *ctx) {
        if (!ctx) return;
        auto &tr = instance();

        std::vector<ImGuiDockNode *> floating_multis;
        for (ImGuiWindow *w : ctx->Windows) {
            if (!w || !w->Name || !is_dock_trackable_tab(w) || !w->Active) continue;
            if (w->DockNode && w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size > 1) {
                if (std::find(floating_multis.begin(), floating_multis.end(), w->DockNode) == floating_multis.end())
                    floating_multis.push_back(w->DockNode);
                auto &wst = tr.float_tracked_[w->ID];
                wst.was_floating = false;
                wst.has_baseline = true;
                continue;
            }
            bool flt = is_window_truly_floating(w);
            auto &st = tr.float_tracked_[w->ID];
            st.name = w->Name;
            st.is_container = false;
            st.was_floating = flt;
            st.dock_id = w->DockNode ? w->DockNode->ID : w->DockId;
            if (flt) {
                st.pos = w->Pos;
                st.size = w->Size;
            }
            st.has_baseline = true;
        }

        for (ImGuiDockNode *n : floating_multis) {
            if (!n) continue;
            auto &cst = tr.float_tracked_[n->ID];
            const char *win_name = nullptr;
            if (subtree_has_builtin_tab(n)) win_name = "ReShade";
            else if (n->VisibleWindow && n->VisibleWindow->Name) win_name = n->VisibleWindow->Name;
            else if (n->Windows.Size > 0 && n->Windows[0] && n->Windows[0]->Name) win_name = n->Windows[0]->Name;
            cst.name = win_name ? win_name : "Panel";
            cst.is_container = true;
            cst.was_floating = true;
            cst.dock_id = n->ID;
            cst.pos = n->Pos;
            cst.size = n->Size;
            cst.has_baseline = true;
        }

        std::vector<ImGuiDockNode *> splits;
        collect_visible_split_nodes(find_viewport_dockspace_root(ctx), splits);
        for (ImGuiDockNode *p : splits) {
            ImGuiAxis axis = p->SplitAxis;
            auto &st = tr.split_tracked_[p->ID];
            st.c0_id = p->ChildNodes[0]->ID;
            st.c1_id = p->ChildNodes[1]->ID;
            st.axis = axis;
            st.parent_size = p->Size[axis];
            st.c0_size = p->ChildNodes[0]->Size[axis];
            st.c1_size = p->ChildNodes[1]->Size[axis];
            st.has_baseline = true;
        }
    }

    std::vector<PendingWindowApply> pending_win_;
    std::vector<PendingSplitApply> pending_split_;
    std::unordered_map<ImGuiID, FloatState> float_tracked_;
    std::unordered_map<ImGuiID, SplitState> split_tracked_;
    ImVec2 last_display_size_ = {};
    int suppress_ = 0;
    int settle_frames_ = 0;
    bool was_dragging_ = false;
    bool defer_ = false;
};

class WindowPosSizeCommand : public ICommand {
public:
    WindowPosSizeCommand() = default;
    WindowPosSizeCommand(std::string name, ImVec2 before_pos, ImVec2 before_size,
                         ImVec2 after_pos, ImVec2 after_size, bool is_container = false,
                         bool apply_pos = true, bool apply_size = true)
        : name_(std::move(name)), bpos_(before_pos), bsize_(before_size),
          apos_(after_pos), asize_(after_size), is_container_(is_container),
          apply_pos_(apply_pos), apply_size_(apply_size) {}

    void execute(reshade::api::effect_runtime * = nullptr) override {
        WindowTransformTracker::apply_window(name_, apos_, asize_, is_container_, apply_pos_, apply_size_);
    }
    void undo(reshade::api::effect_runtime * = nullptr) override {
        WindowTransformTracker::apply_window(name_, bpos_, bsize_, is_container_, apply_pos_, apply_size_);
    }

    CommandDomain domain() const override { return CommandDomain::WindowLayout; }
    uint32_t type_id() const override { return kCommandTypeWindowTransform; }

    bool is_resize() const {
        return apply_size_ && (!apply_pos_ || (std::fabs(bsize_.x - asize_.x) >= 1.0f || std::fabs(bsize_.y - asize_.y) >= 1.0f));
    }

    std::string get_label() const override {
        std::string dn = clean_dock_tab_name(name_);
        char buf[256];
        if (is_resize())
            std::snprintf(buf, sizeof(buf), "Resize '%s' (%.0fx%.0f)", dn.c_str(), asize_.x, asize_.y);
        else
            std::snprintf(buf, sizeof(buf), "Move '%s' (%.0f, %.0f)", dn.c_str(), apos_.x, apos_.y);
        return buf;
    }

    std::string get_delta_preview() const override {
        char buf[128];
        if (is_resize())
            std::snprintf(buf, sizeof(buf), "(%.0fx%.0f) -> (%.0fx%.0f)", bsize_.x, bsize_.y, asize_.x, asize_.y);
        else
            std::snprintf(buf, sizeof(buf), "(%.0f, %.0f) -> (%.0f, %.0f)", bpos_.x, bpos_.y, apos_.x, apos_.y);
        return buf;
    }

private:
    std::string name_;
    ImVec2 bpos_ = {}, bsize_ = {};
    ImVec2 apos_ = {}, asize_ = {};
    bool is_container_ = false;
    bool apply_pos_ = true;
    bool apply_size_ = true;
};

class DockSplitterCommand : public ICommand {
public:
    DockSplitterCommand() = default;
    DockSplitterCommand(std::string label_name, ImGuiID split_id, ImGuiID c0_id, ImGuiID c1_id,
                        ImGuiAxis axis, float before_s0, float before_s1,
                        float after_s0, float after_s1, bool preview_c1 = false)
        : label_name_(std::move(label_name)), split_id_(split_id), c0_id_(c0_id), c1_id_(c1_id),
          axis_(axis), bs0_(before_s0), bs1_(before_s1), as0_(after_s0), as1_(after_s1),
          preview_c1_(preview_c1) {}

    void execute(reshade::api::effect_runtime * = nullptr) override {
        WindowTransformTracker::apply_split(split_id_, c0_id_, c1_id_, axis_, as0_, as1_);
    }
    void undo(reshade::api::effect_runtime * = nullptr) override {
        WindowTransformTracker::apply_split(split_id_, c0_id_, c1_id_, axis_, bs0_, bs1_);
    }

    CommandDomain domain() const override { return CommandDomain::WindowLayout; }
    uint32_t type_id() const override { return kCommandTypeWindowTransform; }

    std::string get_label() const override {
        return "Resize '" + label_name_ + "'";
    }

    std::string get_delta_preview() const override {
        char buf[64];
        float b = preview_c1_ ? bs1_ : bs0_;
        float a = preview_c1_ ? as1_ : as0_;
        std::snprintf(buf, sizeof(buf), "%.0fpx -> %.0fpx", b, a);
        return buf;
    }

private:
    std::string label_name_;
    ImGuiID split_id_ = 0, c0_id_ = 0, c1_id_ = 0;
    ImGuiAxis axis_ = ImGuiAxis_X;
    float bs0_ = 0.0f, bs1_ = 0.0f;
    float as0_ = 0.0f, as1_ = 0.0f;
    bool preview_c1_ = false;
};

inline void WindowTransformTracker::update(ImGuiContext *ctx) {
    if (!ctx) return;
    auto &tr = instance();

    bool mouse_down = ctx->IO.MouseDown[0] || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    if (tr.defer_) {
        if (!mouse_down) {
            tr.defer_ = false;
        } else {
            tr.suppress_ = (std::max)(tr.suppress_, 4);
            return;
        }
    }

    bool applied_any = false;
    if (!tr.pending_win_.empty()) {
        std::vector<PendingWindowApply> batch;
        batch.swap(tr.pending_win_);
        for (const auto &p : batch)
            apply_window_now(ctx, p.name, p.pos, p.size, p.is_container, p.apply_pos, p.apply_size);
        applied_any = true;
    }
    if (!tr.pending_split_.empty()) {
        std::vector<PendingSplitApply> batch;
        batch.swap(tr.pending_split_);
        for (const auto &p : batch)
            apply_split_now(ctx, p.split_id, p.c0_id, p.c1_id, p.axis, p.s0, p.s1);
        applied_any = true;
    }
    if (applied_any) {
        tr.suppress_ = (std::max)(tr.suppress_, 6);
        g_suppress_tracking_frames = 6;
        sync_baselines(ctx);
    }

    bool res_changed = tr.last_display_size_.x > 0.0f && tr.last_display_size_.y > 0.0f &&
        (std::fabs(ctx->IO.DisplaySize.x - tr.last_display_size_.x) >= 1.0f ||
         std::fabs(ctx->IO.DisplaySize.y - tr.last_display_size_.y) >= 1.0f);
    tr.last_display_size_ = ctx->IO.DisplaySize;
    if (res_changed) {
        tr.was_dragging_ = false;
        tr.settle_frames_ = 0;
        sync_baselines(ctx);
        return;
    }

    mouse_down = ctx->IO.MouseDown[0] || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    bool drag_active = mouse_down || ctx->MovingWindow != nullptr || ImGui::GetDragDropPayload() != nullptr;

    if (g_suppress_tracking_frames > 0)
        tr.suppress_ = std::max(tr.suppress_, g_suppress_tracking_frames);
    if (tr.suppress_ > 0) {
        --tr.suppress_;
        if (!drag_active) {
            tr.was_dragging_ = false;
            tr.settle_frames_ = 0;
            sync_baselines(ctx);
            return;
        }
    }
    if (ContextManager::instance().is_suppressing()) {
        if (!drag_active) {
            tr.was_dragging_ = false;
            tr.settle_frames_ = 0;
            sync_baselines(ctx);
        }
        return;
    }

    if (drag_active) {
        if (!tr.was_dragging_) {
            for (ImGuiWindow *w : ctx->Windows) {
                if (!w || !w->Name || !is_dock_trackable_tab(w) || !w->Active) continue;
                if (!tr.float_tracked_[w->ID].has_baseline) {
                    sync_baselines(ctx);
                    break;
                }
            }
        }
        tr.was_dragging_ = true;
        tr.settle_frames_ = 2;
        return;
    }

    if (tr.settle_frames_ > 0) {
        --tr.settle_frames_;
        return;
    }

    bool check_commits = tr.was_dragging_;
    tr.was_dragging_ = false;

    if (check_commits) {
        // 1. Check floating windows & floating multi-tab containers
        std::vector<ImGuiDockNode *> floating_multis;
        for (ImGuiWindow *w : ctx->Windows) {
            if (!w || !w->Name || !is_dock_trackable_tab(w) || !w->Active) continue;
            if (w->DockNode && w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size > 1) {
                if (std::find(floating_multis.begin(), floating_multis.end(), w->DockNode) == floating_multis.end())
                    floating_multis.push_back(w->DockNode);
                continue;
            }
            auto it = tr.float_tracked_.find(w->ID);
            if (it == tr.float_tracked_.end() || !it->second.has_baseline) continue;
            const FloatState &st = it->second;

            bool cur_flt = is_window_truly_floating(w);
            ImGuiID cur_dock = w->DockNode ? w->DockNode->ID : w->DockId;
            if (st.was_floating && cur_flt && st.dock_id == cur_dock) {
                bool pos_diff = std::fabs(w->Pos.x - st.pos.x) >= 1.0f || std::fabs(w->Pos.y - st.pos.y) >= 1.0f;
                bool sz_diff = std::fabs(w->Size.x - st.size.x) >= 1.0f || std::fabs(w->Size.y - st.size.y) >= 1.0f;
                bool track_p = pos_diff && g_capture_window_position;
                bool track_s = sz_diff && g_capture_window_resize;
                if (track_p || track_s) {
                    ImVec2 bpos = track_p ? st.pos : w->Pos;
                    ImVec2 bsz  = track_s ? st.size : w->Size;
                    ContextManager::instance().push_command(std::make_unique<WindowPosSizeCommand>(
                        w->Name, bpos, bsz, w->Pos, w->Size, false, track_p, track_s));
                    tr.suppress_ = g_suppress_tracking_frames = 4;
                }
            }
        }

        for (ImGuiDockNode *n : floating_multis) {
            if (!n) continue;
            auto it = tr.float_tracked_.find(n->ID);
            if (it != tr.float_tracked_.end() && it->second.has_baseline && it->second.was_floating) {
                const FloatState &st = it->second;
                bool pos_diff = std::fabs(n->Pos.x - st.pos.x) >= 1.0f || std::fabs(n->Pos.y - st.pos.y) >= 1.0f;
                bool sz_diff = std::fabs(n->Size.x - st.size.x) >= 1.0f || std::fabs(n->Size.y - st.size.y) >= 1.0f;
                bool track_p = pos_diff && g_capture_window_position;
                bool track_s = sz_diff && g_capture_window_resize;
                if (track_p || track_s) {
                    ImVec2 bpos = track_p ? st.pos : n->Pos;
                    ImVec2 bsz  = track_s ? st.size : n->Size;
                    ContextManager::instance().push_command(std::make_unique<WindowPosSizeCommand>(
                        st.name, bpos, bsz, n->Pos, n->Size, true, track_p, track_s));
                    tr.suppress_ = g_suppress_tracking_frames = 4;
                }
            }
        }

        // 2. Check docked splitters in ViewportDockspace
        if (g_capture_window_resize) {
            std::vector<ImGuiDockNode *> splits;
            collect_visible_split_nodes(find_viewport_dockspace_root(ctx), splits);
            for (ImGuiDockNode *p : splits) {
                if (!p || !p->ChildNodes[0] || !p->ChildNodes[1]) continue;
                auto it = tr.split_tracked_.find(p->ID);
                if (it == tr.split_tracked_.end() || !it->second.has_baseline) continue;
                const SplitState &st = it->second;
                ImGuiAxis axis = p->SplitAxis;
                ImGuiDockNode *c0 = p->ChildNodes[0], *c1 = p->ChildNodes[1];
                if (st.axis != axis || st.c0_id != c0->ID || st.c1_id != c1->ID) continue;

                // Only commit if p's own outer size stayed constant while its two children traded size.
                // This prevents descendant splits from firing false bystander commands when an outer splitter moves.
                bool parent_unchanged = std::fabs(p->Size[axis] - st.parent_size) < 2.0f;
                bool split_changed = std::fabs(c0->Size[axis] - st.c0_size) >= 2.0f &&
                                     std::fabs(c1->Size[axis] - st.c1_size) >= 2.0f;
                if (parent_unchanged && split_changed) {
                    bool use_c1 = c0->HasCentralNodeChild && !c1->HasCentralNodeChild;
                    std::string label = describe_dock_subtree(use_c1 ? c1 : c0);
                    ContextManager::instance().push_command(std::make_unique<DockSplitterCommand>(
                        label, p->ID, c0->ID, c1->ID, axis,
                        st.c0_size, st.c1_size, c0->Size[axis], c1->Size[axis], use_c1));
                    tr.suppress_ = g_suppress_tracking_frames = 4;
                }
            }
        }
    }

    sync_baselines(ctx);
}
