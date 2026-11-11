#pragma once
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cctype>
#define ImTextureID ImU64
#include <imgui.h>
#ifndef GImGui
#define GImGui (ImGui::GetCurrentContext())
#endif
#include <imgui_internal.h>
#include <reshade.hpp>
#include "ICommand.hpp"
#include "ContextManager.hpp"
inline int g_suppress_tracking_frames = 0;
inline bool g_capture_window_docking = false;
inline bool g_capture_tab_docking = true;
inline bool g_capture_window_resize = true;
inline bool g_capture_window_position = true;

namespace {

inline std::string clean_dock_tab_name(const std::string &raw) {
    size_t pos = raw.find("###");
    if (pos == std::string::npos) return raw;
    if (pos > 0) return raw.substr(0, pos);
    std::string sub = raw.substr(3);
    if (!sub.empty()) sub[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(sub[0])));
    return sub.empty() ? raw : sub;
}

inline ImGuiWindow *find_window_by_name(ImGuiContext *ctx, const std::string &name) {
    if (!ctx || name.empty()) return nullptr;
    for (ImGuiWindow *w : ctx->Windows)
        if (w && w->Name && std::strcmp(w->Name, name.c_str()) == 0) return w;
    if (size_t pos = name.find("###"); pos != std::string::npos) {
        const char *tag = name.c_str() + pos;
        for (ImGuiWindow *w : ctx->Windows)
            if (w && w->Name && std::strstr(w->Name, tag)) return w;
    }
    return nullptr;
}

inline bool is_dock_trackable_tab(const ImGuiWindow *w) {
    if (!w || !w->Name || !w->Name[0] || (!w->Active && !w->WasActive) || w->IsExplicitChild || w->DockNodeAsHost)
        return false;
    if (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_ChildMenu))
        return false;
    if (std::strcmp(w->Name, "Viewport") == 0 || std::strcmp(w->Name, "##Viewport") == 0 ||
        std::strncmp(w->Name, "WindowOverViewport_", 19) == 0)
        return false;
    return !(std::strncmp(w->Name, "##", 2) == 0 && !std::strstr(w->Name, "###"));
}

inline bool is_builtin_tab(const ImGuiWindow *w) {
    if (!w || !w->Name) return false;
    for (const char *tag : {"###home", "###settings", "###addons", "###statistics", "###log", "###about"})
        if (std::strstr(w->Name, tag)) return true;
    return false;
}
inline bool is_overlay_tab_node(const ImGuiDockNode *n) {
    if (!n || !n->IsLeafNode() || n->Windows.Size <= 1) return false;
    for (ImGuiWindow *w : n->Windows)
        if (is_builtin_tab(w)) return true;
    return false;
}

inline bool is_overlay_tab(const ImGuiWindow *w) {
    return w && is_overlay_tab_node(w->DockNode);
}

inline bool is_floating_window(const ImGuiWindow *w) {
    return w && (!w->DockNode || (w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size <= 1));
}

// closed or inactive docked windows have DockNode == nullptr but retain DockId != 0; dont treat them as floating
inline bool is_window_truly_floating(const ImGuiWindow *w) {
    if (!w) return false;
    if (is_overlay_tab(w)) return false;
    if (!w->DockNode) return w->DockId == 0;
    return w->DockNode->IsFloatingNode() && w->DockNode->Windows.Size <= 1 && !w->DockNode->ParentNode;
}

inline ImGuiDockNode *resolve_overlay_node(ImGuiContext *ctx, ImGuiID dock_id, const ImGuiWindow *self) {
    if (!ctx) return nullptr;
    if (dock_id != 0) {
        for (ImGuiWindow *c : ctx->Windows)
            if (c && c != self && c->DockNode && c->DockNode->ID == dock_id && is_overlay_tab_node(c->DockNode))
                return c->DockNode;
    }
    for (ImGuiWindow *c : ctx->Windows)
        if (c && c != self && c->DockNode && c->DockNode->IsLeafNode() && is_builtin_tab(c))
            return c->DockNode;
    return nullptr;
}

inline void erase_tab(ImGuiDockNode *node, ImGuiID tab_id) {
    if (!node || !node->TabBar) return;
    auto &tabs = node->TabBar->Tabs;
    for (int i = 0; i < tabs.Size; ++i)
        if (tabs[i].ID == tab_id) { tabs.erase(&tabs[i]); break; }
    ImGuiID fb = tabs.Size > 0 ? tabs[0].ID : 0;
    if (node->TabBar->VisibleTabId == tab_id) node->TabBar->VisibleTabId = fb;
    if (node->TabBar->SelectedTabId == tab_id) node->TabBar->SelectedTabId = fb;
    if (node->TabBar->NextSelectedTabId == tab_id) node->TabBar->NextSelectedTabId = node->TabBar->SelectedTabId;
}

inline void select_tab(ImGuiDockNode *node, ImGuiWindow *w) {
    if (!node || !w) return;
    node->SelectedTabId = w->TabId;
    node->VisibleWindow = w;
    node->WantHiddenTabBarUpdate = true;
    if (node->TabBar)
        node->TabBar->SelectedTabId = node->TabBar->NextSelectedTabId = node->TabBar->VisibleTabId = w->TabId;
}

// Unlinks w in-place without touching ctx->DockContext (whose offset/heap lives in dxgi.dll).
// Removes w from ParentWindow->DC.ChildWindows when clearing ImGuiWindowFlags_ChildWindow
// so ImGui::EndFrame() doesn't assert on g.WindowsTempSortBuffer.Size (imgui.cpp:6217).
inline void detach_window(ImGuiWindow *w) {
    if (!w) return;
    if (ImGuiDockNode *n = w->DockNode) {
        n->Windows.find_erase(w);
        erase_tab(n, w->TabId);
        if (n->VisibleWindow == w) n->VisibleWindow = n->Windows.Size > 0 ? n->Windows[0] : nullptr;
        if (n->SelectedTabId == w->TabId) n->SelectedTabId = (n->Windows.Size > 0 && n->Windows[0]) ? n->Windows[0]->TabId : 0;
        n->WantHiddenTabBarUpdate = true;
        w->DockNode = nullptr;
    }
    w->DockIsActive = w->DockNodeIsVisible = w->DockTabIsVisible = w->DockTabWantClose = false;
    w->Flags &= ~ImGuiWindowFlags_ChildWindow;
    if (w->ParentWindow) {
        w->ParentWindow->DC.ChildWindows.find_erase(w);
        w->ParentWindow = nullptr;
    }
    w->ParentWindowInBeginStack = w->ParentWindowForFocusRoute = nullptr;
    w->RootWindow = w->RootWindowPopupTree = w->RootWindowDockTree = w->RootWindowForTitleBarHighlight = w->RootWindowForNav = w;
}

} // namespace

enum class LayoutDockAction { Dock, Undock };

class TabDockingTracker {
public:
    struct TabState {
        bool is_docked = false;
        ImGuiID dock_id = 0;
        short dock_order = -1;
        ImVec2 floating_pos = {};
        ImVec2 floating_size = {};
        bool has_baseline = false;
    };

    struct PendingApply {
        std::string name;
        bool should_dock = false;
        ImGuiID target_id = 0;
        short order = -1;
        ImVec2 fpos = {}, fsize = {};
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
        return !instance().pending_.empty();
    }

    static void flush_deferred(ImGuiContext *ctx) {
        if (!ctx) return;
        auto &tr = instance();
        tr.defer_ = false;
        if (tr.pending_.empty()) return;
        tr.suppress_ = g_suppress_tracking_frames = 6;
        std::vector<PendingApply> batch;
        batch.swap(tr.pending_);
        for (const auto &p : batch) {
            apply_now(ctx, p.name, p.should_dock, p.target_id, p.order, p.fpos, p.fsize);
        }
        sync_baselines(ctx);
    }

    static void apply(const std::string &name, bool should_dock, ImGuiID target_id, short order, ImVec2 fpos, ImVec2 fsize) {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        instance().suppress_ = g_suppress_tracking_frames = 4;

        auto &pend = instance().pending_;
        pend.erase(std::remove_if(pend.begin(), pend.end(),
            [&](const PendingApply &p) { return p.name == name; }), pend.end());

        if (instance().defer_ || ctx->CurrentWindowStack.Size > 1) {
            pend.push_back({name, should_dock, target_id, order, fpos, fsize});
            return;
        }

        apply_now(ctx, name, should_dock, target_id, order, fpos, fsize);
    }

    static void apply_now(ImGuiContext *ctx, const std::string &name, bool should_dock, ImGuiID target_id, short order, ImVec2 fpos, ImVec2 fsize) {
        ImGuiWindow *w = find_window_by_name(ctx, name);
        if (!w) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "[UndoRedo] Tab dock apply skipped: window '%s' not found.", name.c_str());
            reshade::log::message(reshade::log::level::warning, msg);
            return;
        }
        if (ctx->MovingWindow == w) ctx->MovingWindow = nullptr;

        auto &st = instance().tracked_[w->ID];

        if (should_dock) {
            ImGuiDockNode *target_node = resolve_overlay_node(ctx, target_id, w);
            ImGuiID dest_id = target_node ? target_node->ID : target_id;
            if (dest_id == 0) return;

            ImGuiWindow *keep_win = (target_node && target_node->VisibleWindow && target_node->VisibleWindow != w)
                                    ? target_node->VisibleWindow : w;

            if (w->DockNode && w->DockNode->ID == dest_id) {
                w->DockOrder = order;
                select_tab(target_node, keep_win);
            } else {
                detach_window(w);
                erase_tab(target_node, w->TabId);
                w->DockId = dest_id;
                w->DockOrder = order;
                w->Collapsed = false;
                w->Hidden = true;
                select_tab(target_node, keep_win);
            }
            if (keep_win && keep_win != w && keep_win->Name)
                ImGui::SetWindowFocus(keep_win->Name);

            st.is_docked = true;
            st.dock_id = dest_id;
            if (order >= 0) st.dock_order = order;
        } else {
            detach_window(w);
            w->DockId = 0;
            w->Collapsed = false;
            w->Flags &= ~(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
            w->ChildFlags = 0;
            w->Hidden = true;

            const ImGuiIO &io = ImGui::GetIO();
            float ww = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1920.0f;
            float wh = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 1080.0f;
            ImVec2 raw_sz = (fsize.x > 20.0f && fsize.y > 20.0f) ? fsize : ImVec2(w->Size.x * 0.8f, w->Size.y * 0.8f);
            ImVec2 raw_pos = (fsize.x > 20.0f && fsize.y > 20.0f) ? fpos : ImVec2(60.0f, 60.0f);
            ImVec2 sz(std::clamp(raw_sz.x, 100.0f, ww), std::clamp(raw_sz.y, 50.0f, wh));
            ImVec2 pos(std::clamp(raw_pos.x, 0.0f, ww - std::min(sz.x, 120.0f)), std::clamp(raw_pos.y, 0.0f, wh - 35.0f));

            w->SetWindowPosAllowFlags |= ImGuiCond_Always;
            w->SetWindowSizeAllowFlags |= ImGuiCond_Always;
            w->Pos = pos;
            w->Size = w->SizeFull = sz;
            ImGui::SetWindowPos(w->Name, pos, ImGuiCond_Always);
            ImGui::SetWindowSize(w->Name, sz, ImGuiCond_Always);

            st.is_docked = false;
            st.floating_pos = pos;
            st.floating_size = sz;
        }
        st.has_baseline = true;
    }

    static void update(ImGuiContext *ctx);

private:
    static TabDockingTracker &instance() {
        static TabDockingTracker s;
        return s;
    }

    static void sync_window(ImGuiContext *ctx, ImGuiWindow *w, TabState &st) {
        bool docked = is_overlay_tab(w);
        if (!docked && !w->DockNode && w->DockId != 0 && resolve_overlay_node(ctx, w->DockId, w))
            docked = true;
        st.is_docked = docked;
        if (docked && w->DockNode) {
            st.dock_id = w->DockNode->ID;
            st.dock_order = w->DockOrder;
        } else if (is_floating_window(w)) {
            st.floating_pos = w->Pos;
            st.floating_size = w->Size;
        }
        st.has_baseline = true;
    }

    static void sync_baselines(ImGuiContext *ctx) {
        if (!ctx) return;
        for (ImGuiWindow *w : ctx->Windows)
            if (w && w->Name && is_dock_trackable_tab(w))
                sync_window(ctx, w, instance().tracked_[w->ID]);
    }

    std::vector<PendingApply> pending_;
    std::unordered_map<ImGuiID, TabState> tracked_;
    int suppress_ = 0;
    bool defer_ = false;
};

class TabDockCommand : public ICommand {
public:
    TabDockCommand() = default;
    TabDockCommand(std::string name, std::string /*target_name*/, LayoutDockAction action,
                   ImVec2 fpos, ImVec2 fsize, ImGuiID dock_id = 0, short order = -1)
        : name_(std::move(name)), action_(action), fpos_(fpos), fsize_(fsize), dock_id_(dock_id), order_(order) {}

    void execute(reshade::api::effect_runtime * = nullptr) override {
        TabDockingTracker::apply(name_, action_ == LayoutDockAction::Dock, dock_id_, order_, fpos_, fsize_);
    }
    void undo(reshade::api::effect_runtime * = nullptr) override {
        TabDockingTracker::apply(name_, action_ == LayoutDockAction::Undock, dock_id_, order_, fpos_, fsize_);
    }

    CommandDomain domain() const override { return CommandDomain::WindowLayout; }
    uint32_t type_id() const override { return kCommandTypeWindowTransform; }

    std::string get_label() const override {
        return (action_ == LayoutDockAction::Undock ? "Undock '" : "Dock '") + clean_dock_tab_name(name_) + "'";
    }

    std::string get_delta_preview() const override {
        return action_ == LayoutDockAction::Undock ? "Docked -> Floating" : "Floating -> Docked";
    }

private:
    std::string name_;
    LayoutDockAction action_ = LayoutDockAction::Undock;
    ImVec2 fpos_ = {}, fsize_ = {};
    ImGuiID dock_id_ = 0;
    short order_ = -1;
};

inline void TabDockingTracker::update(ImGuiContext *ctx) {
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

    if (!tr.pending_.empty()) {
        std::vector<PendingApply> batch;
        batch.swap(tr.pending_);
        for (const auto &p : batch) {
            apply_now(ctx, p.name, p.should_dock, p.target_id, p.order, p.fpos, p.fsize);
        }
        tr.suppress_ = (std::max)(tr.suppress_, 6);
        g_suppress_tracking_frames = 6;
        sync_baselines(ctx);
    }

    if (g_suppress_tracking_frames > 0) {
        tr.suppress_ = std::max(tr.suppress_, g_suppress_tracking_frames);
        --g_suppress_tracking_frames;
    }
    if (tr.suppress_ > 0) {
        if (--tr.suppress_ == 0) sync_baselines(ctx);
        return;
    }
    if (ContextManager::instance().is_suppressing()) {
        sync_baselines(ctx);
        return;
    }

    mouse_down = ctx->IO.MouseDown[0] || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    for (ImGuiWindow *w : ctx->Windows) {
        if (w && w->Name && is_dock_trackable_tab(w) && !tr.tracked_[w->ID].has_baseline)
            sync_window(ctx, w, tr.tracked_[w->ID]);
    }

    if (mouse_down)
        return;

    for (ImGuiWindow *w : ctx->Windows) {
        if (!w || !w->Name || !is_dock_trackable_tab(w)) continue;
        auto &st = tr.tracked_[w->ID];
        bool docked = is_overlay_tab(w);
        bool floating = is_floating_window(w);

        if (g_capture_tab_docking) {
            if (st.is_docked && floating) {
                ContextManager::instance().push_command(std::make_unique<TabDockCommand>(
                    w->Name, "", LayoutDockAction::Undock, w->Pos, w->Size, st.dock_id, st.dock_order));
                tr.suppress_ = g_suppress_tracking_frames = 2;
            } else if (!st.is_docked && docked) {
                ContextManager::instance().push_command(std::make_unique<TabDockCommand>(
                    w->Name, "", LayoutDockAction::Dock, st.floating_pos, st.floating_size, w->DockNode->ID, w->DockOrder));
                tr.suppress_ = g_suppress_tracking_frames = 2;
            }
        }
        sync_window(ctx, w, st);
    }
}
