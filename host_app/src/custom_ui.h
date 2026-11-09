#pragma once
#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

struct ShaderLabUniform {
    enum class Type { Float, Int, Bool, Color3, Color4, Enum };
    std::string name;
    std::string label;
    std::string tooltip;
    std::string category;
    Type type = Type::Float;
    float min_val = 0.0f;
    float max_val = 1.0f;
    float step = 0.01f;
    float float_val = 0.5f;
    float default_float = 0.5f;
    int int_val = 0;
    int default_int = 0;
    bool bool_val = false;
    bool default_bool = false;
    float color_val[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float default_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<std::string> enum_items;
    int enum_selected = 0;
    int default_enum = 0;

    void reset_to_default() {
        float_val = default_float;
        int_val = default_int;
        bool_val = default_bool;
        for (int i = 0; i < 4; ++i) color_val[i] = default_color[i];
        enum_selected = default_enum;
    }
};

struct ShaderLabEffect {
    std::string name;
    std::string file_path;
    std::string engine_type;
    bool enabled = true;
    std::vector<ShaderLabUniform> uniforms;

    void reset_uniforms() {
        for (auto &u : uniforms) {
            u.reset_to_default();
        }
    }
};

struct UiDomNode {
    std::string id;
    std::string label;
    std::string type;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    bool active = false;
    bool enabled = false;
};

class UiDomRegistry {
public:
    void clear() { m_nodes.clear(); }
    void register_item(const std::string &id, const std::string &label, const std::string &type,
                       const ImVec2 &p_min, const ImVec2 &p_max, bool active = false, bool enabled = true) {
        UiDomNode node;
        node.id = id;
        node.label = label;
        node.type = type;
        node.x = p_min.x;
        node.y = p_min.y;
        node.w = p_max.x - p_min.x;
        node.h = p_max.y - p_min.y;
        node.active = active;
        node.enabled = enabled;
        m_nodes.push_back(node);
    }

    const std::vector<UiDomNode> &get_nodes() const { return m_nodes; }

    const UiDomNode *find(const std::string &query) const {
        for (const auto &n : m_nodes) {
            if (n.id == query) return &n;
        }
        for (const auto &n : m_nodes) {
            if (_stricmp(n.id.c_str(), query.c_str()) == 0) return &n;
        }
        // Aliases for tools and tabs
        if (query == "tool_hand" || query == "tool_h") return find("tool_H");
        if (query == "tool_erase" || query == "tool_e") return find("tool_E");
        if (query == "tool_depth" || query == "tool_d") return find("tool_D");
        if (query == "tool_split" || query == "tool_b") return find("tool_B");
        if (query == "tool_bypass" || query == "tool_\\") return find("tool_\\");
        if (query == "accordion_depth") {
            const UiDomNode *n = find("tab_depth");
            if (n) return n;
        }
        if (query == "accordion_erase") {
            const UiDomNode *n = find("tab_erase");
            if (n) return n;
        }
        if (query == "opt_fit") { const UiDomNode *n = find("status_btn_fit"); if (n) return n; }
        if (query == "opt_100") { const UiDomNode *n = find("status_btn_100"); if (n) return n; }
        if (query == "opt_200") { const UiDomNode *n = find("status_btn_200"); if (n) return n; }
        for (const auto &n : m_nodes) {
            if (_stricmp(n.label.c_str(), query.c_str()) == 0) return &n;
        }
        for (const auto &n : m_nodes) {
            if (n.label.find(query) != std::string::npos) return &n;
        }
        return nullptr;
    }

    std::string to_json() const {
        std::string out = "{\n  \"elements\": [\n";
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            const auto &n = m_nodes[i];
            out += "    {\"id\": \"" + json_escape(n.id) + "\", ";
            out += "\"label\": \"" + json_escape(n.label) + "\", ";
            out += "\"type\": \"" + json_escape(n.type) + "\", ";
            out += "\"rect\": [" + std::to_string((int)n.x) + ", " + std::to_string((int)n.y) + ", " +
                   std::to_string((int)n.w) + ", " + std::to_string((int)n.h) + "], ";
            out += "\"active\": " + std::string(n.active ? "true" : "false") + ", ";
            out += "\"enabled\": " + std::string(n.enabled ? "true" : "false") + "}";
            if (i + 1 < m_nodes.size()) out += ",";
            out += "\n";
        }
        out += "  ]\n}\n";
        return out;
    }

private:
    static std::string json_escape(const std::string &s) {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    }

    std::vector<UiDomNode> m_nodes;
};

class CustomUI {
public:
    CustomUI();
    ~CustomUI();

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
    float get_tool_options_height() const { return 30.0f; }
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

    enum class ActiveTool {
        Hand,
        Erase,
        Depth,
        Split
    };
    ActiveTool get_active_tool() const { return m_active_tool; }
    void set_active_tool(ActiveTool tool) {
        m_active_tool = tool;
        m_erase_active = (tool == ActiveTool::Erase);
        m_before_after = (tool == ActiveTool::Split);
        m_depth_peek   = (tool == ActiveTool::Depth);
        if (tool == ActiveTool::Erase) {
            m_right_panel_tab = 2;
            m_sync_tab_req = 2;
        } else if (tool == ActiveTool::Depth) {
            m_right_panel_tab = 1;
            m_sync_tab_req = 1;
        } else if (tool == ActiveTool::Hand) {
            m_right_panel_tab = 0;
            m_sync_tab_req = 0;
        }
    }
    void select_right_panel_tab(int tab_idx) {
        m_right_panel_tab = tab_idx;
        m_sync_tab_req = tab_idx;
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

    bool click_dom_element(const std::string &id_or_label) {
        if (id_or_label == "tab_shaders") { select_right_panel_tab(0); return true; }
        if (id_or_label == "tab_depth" || id_or_label == "accordion_depth") { select_right_panel_tab(1); return true; }
        if (id_or_label == "tab_erase" || id_or_label == "accordion_erase") { select_right_panel_tab(2); return true; }
        if (id_or_label == "tool_hand" || id_or_label == "tool_H" || id_or_label == "tool_h") { set_active_tool(ActiveTool::Hand); return true; }
        if (id_or_label == "tool_erase" || id_or_label == "tool_E" || id_or_label == "tool_e") { set_active_tool(ActiveTool::Erase); return true; }
        if (id_or_label == "tool_depth" || id_or_label == "tool_D" || id_or_label == "tool_d") { set_active_tool(ActiveTool::Depth); return true; }
        if (id_or_label == "tool_split" || id_or_label == "tool_B" || id_or_label == "tool_b") { set_active_tool(ActiveTool::Split); return true; }
        if (id_or_label == "tool_bypass" || id_or_label == "tool_\\") { m_master_bypass = !m_master_bypass; return true; }
        if (id_or_label == "opt_fit" || id_or_label == "status_btn_fit") { m_req_fit_view = true; return true; }
        if (id_or_label == "opt_100" || id_or_label == "status_btn_100") { m_req_100_view = true; return true; }
        if (id_or_label == "opt_200" || id_or_label == "status_btn_200") { m_req_200_view = true; return true; }
        if (id_or_label == "opt_static_zoom" || id_or_label == "menu_opt_static_zoom") { m_static_zoom = !m_static_zoom; return true; }
        if (id_or_label == "opt_undo" || id_or_label == "erase_btn_undo") { undo_erase_stroke(); return true; }
        if (id_or_label == "opt_redo") { redo_erase_stroke(); return true; }
        if (id_or_label == "opt_clear" || id_or_label == "erase_btn_clear") { clear_erase_strokes(); m_req_clear_mask = true; return true; }
        if (id_or_label == "erase_checkbox_subtract" || id_or_label == "opt_subtract") { m_erase_subtract = !m_erase_subtract; return true; }

        const UiDomNode *n = m_dom.find(id_or_label);
        if (!n) return false;
        if (n->id == "tab_shaders") { select_right_panel_tab(0); return true; }
        if (n->id == "tab_depth" || n->id == "accordion_depth") { select_right_panel_tab(1); return true; }
        if (n->id == "tab_erase" || n->id == "accordion_erase") { select_right_panel_tab(2); return true; }

        float cx = n->x + n->w * 0.5f;
        float cy = n->y + n->h * 0.5f;
        queue_mouse_event(cx, cy, 1);
        return true;
    }

    const std::vector<std::vector<ImVec2>> &get_erase_strokes() const { return m_erase_strokes; }
    void clear_erase_strokes();
    bool undo_erase_stroke();
    bool redo_erase_stroke();
    void push_erase_undo();
    void subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y);

    std::vector<ShaderLabEffect> &get_effects() { return m_effects; }
    const std::vector<ShaderLabEffect> &get_effects() const { return m_effects; }

    bool pop_add_shader_request(std::wstring &out_path) {
        if (!m_req_add_shader) return false;
        m_req_add_shader = false;
        out_path = m_add_shader_path;
        return true;
    }
    void simulate_canvas_mouse(float x, float y, int act);

private:
    void apply_photoshop_gray_theme();
    void render_menu_bar();
    void render_tool_strip(float top_offset, float height);
    void render_tool_options_bar(float top_offset, float width);
    void render_left_panel(float left_offset, float top_offset, float panel_width, float panel_height);
    void render_center_canvas(float left_offset, float top_offset, float canvas_width, float canvas_height,
                              uint32_t img_w, uint32_t img_h,
                              float zoom_level, float pan_x, float pan_y);
    void render_right_panel(float right_offset, float top_offset, float panel_width, float panel_height);
    void render_bottom_status_bar(float top_offset, float width, float height,
                                  uint32_t img_w, uint32_t img_h,
                                  float zoom_level, float pan_x, float pan_y);
    void add_sample_effects();

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
    int m_open_accordion = 0; // 0=none, 1=depth, 2=erase
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
    UiDomRegistry m_dom;

    int m_right_panel_tab = 0;
    int m_sync_tab_req = -1;
    std::vector<ShaderLabEffect> m_effects;
    int m_selected_effect_idx = 0;
    char m_search_buffer[128] = {};
    bool m_show_about_dialog = false;
};
