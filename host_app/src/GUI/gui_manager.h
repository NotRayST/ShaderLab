#pragma once
#include "types.h"
#include <vector>
#include <string>

namespace gui {

class GuiManager {
public:
    using ActiveTool = gui::ActiveTool;
    using DocumentTab = gui::DocumentTab;

    GuiManager();
    ~GuiManager();

    bool initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context);
    void shutdown();

    static bool input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam);

    void begin_frame();
    void render(uint32_t screen_w, uint32_t screen_h,
                uint32_t img_w, uint32_t img_h,
                float zoom_level, float pan_x, float pan_y);
    void end_frame();

    bool is_initialized() const { return m_initialized; }
    bool want_capture_mouse() const;
    bool want_capture_keyboard() const;

    float get_left_panel_width(uint32_t screen_w) const;
    float get_right_panel_width(uint32_t screen_w) const;
    float get_menu_bar_height() const;
    float get_toolbar_width() const { return 36.0f; }
    float get_tab_bar_height() const { return 30.0f; }
    float get_tool_options_height() const { return get_tab_bar_height(); }
    float get_status_bar_height() const { return 24.0f; }

    bool is_depth_peek_active() const { return m_active_tool == ActiveTool::Depth && m_depth_peek; }
    void set_depth_peek_active(bool p) {
        if (p) set_active_tool(ActiveTool::Depth);
        else if (m_active_tool == ActiveTool::Depth) set_active_tool(ActiveTool::Hand);
    }
    float get_depth_far_plane() const { return m_depth_far_plane; }
    void set_depth_far_plane(float f) { m_depth_far_plane = f; }
    bool is_depth_invert() const { return m_depth_invert; }

    void set_depth_status(bool has_depth, uint32_t w, uint32_t h, float far_plane) {
        m_has_depth = has_depth;
        m_depth_w = w;
        m_depth_h = h;
        m_depth_far_plane = far_plane;
    }

    bool is_static_zoom_active() const { return m_static_zoom; }
    void set_static_zoom_active(bool s) { m_static_zoom = s; }

    bool is_before_after_active() const { return m_active_tool == ActiveTool::Split && m_before_after; }
    void set_before_after_active(bool b) {
        if (b) set_active_tool(ActiveTool::Split);
        else if (m_active_tool == ActiveTool::Split) set_active_tool(ActiveTool::Hand);
    }
    float get_before_after_split() const { return m_before_after_split; }
    float get_before_after_angle() const { return m_before_after_angle; }
    void set_before_after_split(float s) { m_before_after_split = s; }

    bool is_master_bypass_active() const { return m_master_bypass; }
    void set_master_bypass(bool b) { m_master_bypass = b; }

    ActiveTool get_active_tool() const { return m_active_tool; }
    void set_active_tool(ActiveTool tool) {
        m_active_tool = tool;
        m_erase_active = (tool == ActiveTool::Erase);
        m_before_after = (tool == ActiveTool::Split);
        m_depth_peek   = (tool == ActiveTool::Depth);
        if (tool == ActiveTool::Erase) {
            m_right_panel_tab = 2;
        } else if (tool == ActiveTool::Depth) {
            m_right_panel_tab = 1;
        } else if (tool == ActiveTool::Hand) {
            m_right_panel_tab = 0;
        }
    }

    void select_right_panel_tab(int tab_idx) {
        m_right_panel_tab = tab_idx;
        if (tab_idx == 0) {
            if (m_active_tool == ActiveTool::Erase || m_active_tool == ActiveTool::Depth) {
                set_active_tool(ActiveTool::Hand);
            }
        } else if (tab_idx == 1) {
            set_active_tool(ActiveTool::Depth);
        } else if (tab_idx == 2) {
            set_active_tool(ActiveTool::Erase);
        }
    }
    int get_right_panel_tab() const { return m_right_panel_tab; }

    bool are_panels_hidden() const { return m_hide_panels; }
    void toggle_panels_hidden() { m_hide_panels = !m_hide_panels; }

    bool is_erase_active() const { return m_active_tool == ActiveTool::Erase && m_erase_active; }
    void set_erase_active(bool e) {
        if (e) set_active_tool(ActiveTool::Erase);
        else if (m_active_tool == ActiveTool::Erase) set_active_tool(ActiveTool::Hand);
    }
    float get_erase_brush_radius() const { return m_erase_radius; }
    bool is_erase_subtract() const { return m_erase_subtract; }

    bool pop_open_image_request(std::wstring &out_path) {
        if (!m_req_open_image) return false;
        m_req_open_image = false;
        out_path = m_open_image_path;
        return true;
    }

    bool pop_save_snapshot_request(std::wstring &out_path) {
        if (!m_req_save_snapshot) return false;
        m_req_save_snapshot = false;
        out_path = m_save_snapshot_path;
        return true;
    }

    bool pop_attach_depth_request(std::wstring &out_path) {
        if (!m_req_attach_depth) return false;
        m_req_attach_depth = false;
        out_path = m_attach_depth_path;
        return true;
    }

    bool pop_reset_view_request() {
        bool req = m_req_reset_view;
        m_req_reset_view = false;
        return req;
    }

    bool pop_fit_view_request() {
        bool req = m_req_fit_view;
        m_req_fit_view = false;
        return req;
    }

    bool pop_100_view_request() {
        bool req = m_req_100_view;
        m_req_100_view = false;
        return req;
    }

    bool pop_200_view_request() {
        bool req = m_req_200_view;
        m_req_200_view = false;
        return req;
    }

    bool pop_clear_mask_request() {
        bool req = m_req_clear_mask;
        m_req_clear_mask = false;
        return req;
    }

    bool pop_generate_depth_request(std::string &out_encoder, float &out_far_plane) {
        if (!m_req_generate_depth) return false;
        m_req_generate_depth = false;
        out_encoder = m_depth_encoder;
        out_far_plane = m_depth_far_plane;
        return true;
    }

    void set_depth_status_text(const std::string &status, bool is_busy = false) {
        m_depth_status_text = status;
        m_depth_is_busy = is_busy;
    }

    bool pop_inpaint_request(std::vector<std::vector<ImVec2>> &out_strokes, float &out_radius) {
        if (!m_req_inpaint) return false;
        m_req_inpaint = false;
        out_strokes = m_erase_strokes;
        out_radius = m_erase_radius;
        return true;
    }

    void set_inpaint_status_text(const std::string &status, bool is_busy = false) {
        m_inpaint_status_text = status;
        m_inpaint_is_busy = is_busy;
    }

    struct SimMouseEvent { float x; float y; int action; };
    void queue_mouse_event(float x, float y, int action) {
        m_sim_mouse_queue.push_back({ x, y, action });
    }

    void register_dom_last_item(const std::string &id, const std::string &label, const std::string &type, bool active = false, bool enabled = true) {
        m_dom.register_item(id, label, type, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), active, enabled);
    }
    const UiDomRegistry &get_dom() const { return m_dom; }
    std::string get_dom_json() const { return m_dom.to_json(); }

    bool click_dom_element(const std::string &id_or_label);

    void set_tabs(const std::vector<DocumentTab> &tabs, int active_index) {
        m_tabs = tabs;
        m_active_tab = active_index;
    }

    void select_tab(int index) {
        m_programmatic_select_tab = index;
        m_active_tab = index;
    }

    int get_active_tab() const { return m_active_tab; }
    const std::vector<DocumentTab> &get_tabs() const { return m_tabs; }

    void request_close_active_tab() {
        if (m_active_tab >= 0 && m_active_tab < static_cast<int>(m_tabs.size())) {
            m_req_close_tab = m_active_tab;
        }
    }

    void request_cycle_tab(int dir) {
        if (m_tabs.empty()) return;
        int count = static_cast<int>(m_tabs.size());
        int next = (m_active_tab + dir) % count;
        if (next < 0) next += count;
        m_req_switch_tab = next;
    }

    bool pop_switch_tab_request(int &out_tab_idx) {
        if (m_req_switch_tab < 0) return false;
        out_tab_idx = m_req_switch_tab;
        m_req_switch_tab = -1;
        return true;
    }

    bool pop_close_tab_request(int &out_tab_idx) {
        if (m_req_close_tab < 0) return false;
        out_tab_idx = m_req_close_tab;
        m_req_close_tab = -1;
        return true;
    }

    bool pop_close_all_tabs_request() {
        if (!m_req_close_all_tabs) return false;
        m_req_close_all_tabs = false;
        return true;
    }

    bool pop_close_other_tabs_request(int &out_keep_idx) {
        if (m_req_close_other_tabs < 0) return false;
        out_keep_idx = m_req_close_other_tabs;
        m_req_close_other_tabs = -1;
        return true;
    }

    bool pop_duplicate_tab_request(int &out_tab_idx) {
        if (m_req_duplicate_tab < 0) return false;
        out_tab_idx = m_req_duplicate_tab;
        m_req_duplicate_tab = -1;
        return true;
    }

    bool pop_new_tab_request() {
        if (!m_req_new_tab) return false;
        m_req_new_tab = false;
        return true;
    }

    const std::vector<std::vector<ImVec2>> &get_erase_strokes() const { return m_erase_strokes; }
    const std::vector<std::vector<std::vector<ImVec2>>> &get_erase_undo_stack() const { return m_erase_undo_stack; }
    const std::vector<std::vector<std::vector<ImVec2>>> &get_erase_redo_stack() const { return m_erase_redo_stack; }
    void set_erase_strokes(const std::vector<std::vector<ImVec2>> &strokes,
                           const std::vector<std::vector<std::vector<ImVec2>>> &undo_stack,
                           const std::vector<std::vector<std::vector<ImVec2>>> &redo_stack) {
        m_erase_strokes = strokes;
        m_erase_undo_stack = undo_stack;
        m_erase_redo_stack = redo_stack;
        m_current_stroke.clear();
        m_is_painting_mask = false;
    }

    void clear_erase_strokes();
    bool undo_erase_stroke();
    bool redo_erase_stroke();
    void push_erase_undo();
    void subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y);

    std::vector<ShaderLabEffect> &get_effects() { return m_effects; }
    const std::vector<ShaderLabEffect> &get_effects() const { return m_effects; }
    void set_effects(const std::vector<ShaderLabEffect> &effects) {
        m_effects = effects;
    }

    bool pop_add_shader_request(std::wstring &out_path) {
        if (!m_req_add_shader) return false;
        m_req_add_shader = false;
        out_path = m_add_shader_path;
        return true;
    }
    void simulate_canvas_mouse(float x, float y, int act);

    // Public state accessors for component renderers
    HWND get_hwnd() const { return m_hwnd; }
    UiDomRegistry &get_dom_mut() { return m_dom; }

    // Friend component renderers
    friend void render_menu_bar(GuiManager &gui);
    friend void render_tool_strip(GuiManager &gui, float top_offset, float height);
    friend void render_tab_bar(GuiManager &gui, float x_offset, float top_offset, float width);
    friend void render_left_panel(GuiManager &gui, float left_offset, float top_offset, float panel_width, float panel_height);
    friend void render_right_panel(GuiManager &gui, float right_offset, float top_offset, float panel_width, float panel_height);
    friend void render_center_canvas(GuiManager &gui, float left_offset, float top_offset, float canvas_width, float canvas_height,
                                     uint32_t img_w, uint32_t img_h, float zoom_level, float pan_x, float pan_y);
    friend void render_bottom_status_bar(GuiManager &gui, float top_offset, float width, float height,
                                         uint32_t img_w, uint32_t img_h, float zoom_level, float pan_x, float pan_y);

private:
    HWND m_hwnd = nullptr;
    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    bool m_initialized = false;

    float m_left_panel_width = 280.0f;
    float m_right_panel_width = 340.0f;
    bool m_resizing_left = false;
    bool m_resizing_right = false;
    bool m_hide_panels = false;
    ActiveTool m_active_tool = ActiveTool::Hand;

    bool m_depth_peek = false;
    float m_depth_far_plane = 1000.0f;
    bool m_depth_invert = false;
    bool m_has_depth = false;
    uint32_t m_depth_w = 0;
    uint32_t m_depth_h = 0;

    bool m_static_zoom = false;
    bool m_before_after = false;
    float m_before_after_split = 0.0f;
    float m_before_after_angle = 0.0f;
    bool m_master_bypass = false;
    bool m_is_dragging_split = false;

    bool m_erase_active = false;
    float m_erase_radius = 24.0f;
    bool m_erase_subtract = false;
    std::vector<std::vector<ImVec2>> m_erase_strokes;
    std::vector<ImVec2> m_current_stroke;
    std::vector<std::vector<std::vector<ImVec2>>> m_erase_undo_stack;
    std::vector<std::vector<std::vector<ImVec2>>> m_erase_redo_stack;
    bool m_is_painting_mask = false;

    bool m_req_open_image = false;
    std::wstring m_open_image_path;
    bool m_req_save_snapshot = false;
    std::wstring m_save_snapshot_path;
    bool m_req_attach_depth = false;
    std::wstring m_attach_depth_path;
    bool m_req_reset_view = false;
    bool m_req_fit_view = false;
    bool m_req_100_view = false;
    bool m_req_200_view = false;
    bool m_req_clear_mask = false;
    bool m_req_add_shader = false;
    std::wstring m_add_shader_path;

    std::vector<DocumentTab> m_tabs;
    int m_active_tab = -1;
    int m_req_switch_tab = -1;
    int m_req_close_tab = -1;
    bool m_req_close_all_tabs = false;
    int m_req_close_other_tabs = -1;
    int m_req_duplicate_tab = -1;
    bool m_req_new_tab = false;
    int m_programmatic_select_tab = -1;

    float m_canvas_view_x = 0.0f;
    float m_canvas_view_y = 0.0f;
    float m_canvas_view_w = 0.0f;
    float m_canvas_view_h = 0.0f;
    uint32_t m_last_img_w = 0;
    uint32_t m_last_img_h = 0;
    float m_last_zoom = 1.0f;
    float m_last_pan_x = 0.0f;
    float m_last_pan_y = 0.0f;

    bool m_req_generate_depth = false;
    std::string m_depth_encoder = "vitl";
    std::string m_depth_status_text;
    bool m_depth_is_busy = false;

    bool m_req_inpaint = false;
    std::string m_inpaint_status_text;
    bool m_inpaint_is_busy = false;
    std::vector<SimMouseEvent> m_sim_mouse_queue;
    bool m_sim_mouse_down_prev = false;
    UiDomRegistry m_dom;

    int m_right_panel_tab = 0;
    std::vector<ShaderLabEffect> m_effects;
    int m_selected_effect_idx = 0;
    char m_search_buffer[128] = {};
    bool m_show_about_dialog = false;
};

} // namespace gui
