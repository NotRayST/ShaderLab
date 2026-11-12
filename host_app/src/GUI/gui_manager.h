#pragma once

#include "types.h"
#include "../crop_rotate_tool.h"
#include "../raster_eraser.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gui {

struct LayerStatusView {
    bool valid = false;
    int active_index = 0;
    int count = 0;
    std::string layer_name = "Background";
    std::string blend_mode_name = "Normal";
    int opacity_pct = 100;
    int fill_pct = 100;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float rotation_deg = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    uint32_t width = 0;
    uint32_t height = 0;
    bool visible = true;
    bool lock_alpha = false;
    bool lock_pixels = false;
    bool lock_position = false;
};

struct LayerPixelView {
    std::vector<uint8_t> *rgba_pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    bool lock_alpha = false;
    bool lock_pixels = false;
    bool is_text_layer = false;
};

struct CheckerboardConfig {
    float checker_size = 8.0f;
    float checker_color_a[4] = { 0.80f, 0.80f, 0.80f, 1.0f };
    float checker_color_b[4] = { 0.60f, 0.60f, 0.60f, 1.0f };
};

struct TextCaretPreview {
    bool active = false;
    float doc_x = 0.0f;
    float doc_y = 0.0f;
    float doc_w = 0.0f;
    float doc_h = 0.0f;
    bool editing = false;
};

class GuiManager {
public:
    using ActiveTool = gui::ActiveTool;
    using DocumentTab = gui::DocumentTab;

    GuiManager();
    ~GuiManager();

    bool initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context);
    void shutdown();

    struct ViewportCallbacks {
        bool (*is_panning)(void *ctx) = nullptr;
        bool (*is_capturing)(void *ctx) = nullptr;
        bool (*handle_input)(void *ctx, UINT msg, WPARAM wParam, LPARAM lParam) = nullptr;
        void (*cancel_input)(void *ctx) = nullptr;
        void (*set_space_pan)(void *ctx, bool active) = nullptr;
        void *user_data = nullptr;
    };
    void set_viewport_callbacks(const ViewportCallbacks &cb) { m_vp_callbacks = cb; }

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
    float get_context_bar_height() const { return 28.0f; }
    // Combined top chrome below the menu bar (Document Tab Bar + Tool Options Context Bar)
    float get_tool_options_height() const { return get_tab_bar_height() + get_context_bar_height(); }
    float get_status_bar_height() const { return 24.0f; }

    float get_right_split_ratio() const { return m_right_split_ratio; }
    void set_right_split_ratio(float ratio) { m_right_split_ratio = std::clamp(ratio, 0.20f, 0.80f); }

    bool is_depth_peek_active() const {
        return (m_active_tool == ActiveTool::Depth && m_depth_peek) || m_live_depth_peek || m_depth_key_held;
    }
    void set_depth_peek_active(bool p) { m_live_depth_peek = p; }
    bool is_live_depth_peek() const { return m_live_depth_peek; }
    void set_live_depth_peek(bool p) { m_live_depth_peek = p; }
    void set_depth_key_held(bool held) { m_depth_key_held = held; }
    bool is_depth_key_held() const { return m_depth_key_held; }
    float get_depth_far_plane() const { return m_depth_far_plane; }
    void set_depth_far_plane(float f) { m_depth_far_plane = f; }
    bool is_depth_invert() const { return m_depth_invert; }
    void set_depth_invert(bool inv) { m_depth_invert = inv; }

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
    void set_active_tool(ActiveTool tool);
    void select_right_panel_tab(int tab_idx);
    int get_right_panel_tab() const { return m_right_panel_tab; }

    bool are_panels_hidden() const { return m_hide_panels; }
    void toggle_panels_hidden() { m_hide_panels = !m_hide_panels; }

    bool is_erase_active() const { return m_active_tool == ActiveTool::AiInpaint && m_erase_active; }
    void set_erase_active(bool e) {
        if (e) set_active_tool(ActiveTool::AiInpaint);
        else if (m_active_tool == ActiveTool::AiInpaint) set_active_tool(ActiveTool::Hand);
    }
    float get_erase_brush_radius() const { return m_erase_radius; }
    bool is_erase_subtract() const { return m_erase_subtract || m_is_right_click_subtract; }

    // File / Project / Lifecycle / Preferences request flags & pop helpers
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

    bool pop_save_project_request() {
        const bool req = m_req_save_project;
        m_req_save_project = false;
        return req;
    }

    bool pop_save_project_as_request(std::wstring &out_path) {
        if (!m_req_save_project_as) return false;
        m_req_save_project_as = false;
        out_path = m_save_project_path;
        return true;
    }

    bool pop_export_image_request() {
        const bool req = m_req_export_image;
        m_req_export_image = false;
        return req;
    }

    bool pop_export_image_as_request(std::wstring &out_path) {
        if (!m_req_export_image_as) return false;
        m_req_export_image_as = false;
        out_path = m_export_image_path;
        return true;
    }

    bool pop_open_prefs_request() {
        const bool req = m_req_open_prefs;
        m_req_open_prefs = false;
        return req;
    }

    bool pop_save_and_exit_request() {
        const bool req = m_req_save_and_exit;
        m_req_save_and_exit = false;
        return req;
    }

    bool pop_force_exit_without_saving_request() {
        const bool req = m_req_force_exit_without_saving;
        m_req_force_exit_without_saving = false;
        return req;
    }

    void open_unsaved_exit_modal() {
        m_show_unsaved_exit_modal = true;
        m_unsaved_exit_modal_open = true;
    }

    void close_unsaved_exit_modal() {
        m_show_unsaved_exit_modal = false;
        m_unsaved_exit_modal_open = false;
    }

    bool is_unsaved_exit_modal_open() const {
        return m_show_unsaved_exit_modal || m_unsaved_exit_modal_open;
    }

    void mark_active_document_dirty();

    bool pop_attach_depth_request(std::wstring &out_path) {
        if (!m_req_attach_depth) return false;
        m_req_attach_depth = false;
        out_path = m_attach_depth_path;
        return true;
    }

    bool pop_reset_view_request() {
        const bool req = m_req_reset_view;
        m_req_reset_view = false;
        return req;
    }

    bool pop_fit_view_request() {
        const bool req = m_req_fit_view;
        m_req_fit_view = false;
        return req;
    }

    bool pop_100_view_request() {
        const bool req = m_req_100_view;
        m_req_100_view = false;
        return req;
    }

    bool pop_200_view_request() {
        const bool req = m_req_200_view;
        m_req_200_view = false;
        return req;
    }

    bool pop_zoom_in_request() {
        const bool req = m_req_zoom_in;
        m_req_zoom_in = false;
        return req;
    }

    bool pop_zoom_out_request() {
        const bool req = m_req_zoom_out;
        m_req_zoom_out = false;
        return req;
    }

    bool pop_clear_mask_request() {
        const bool req = m_req_clear_mask;
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

    // Rotate / Flip / Crop / Move polling helpers (alongside optional direct callbacks)
    bool pop_rotate_angle_request(float &out_angle_deg, bool &out_constrain_scale) {
        if (!m_req_set_viewport_angle) return false;
        m_req_set_viewport_angle = false;
        out_angle_deg = m_target_viewport_angle;
        out_constrain_scale = m_horizon_state.auto_scale_to_fit;
        return true;
    }

    bool pop_flip_horizontal_request() {
        const bool req = m_req_flip_horizontal;
        m_req_flip_horizontal = false;
        return req;
    }

    bool pop_flip_vertical_request() {
        const bool req = m_req_flip_vertical;
        m_req_flip_vertical = false;
        return req;
    }

    bool pop_move_delta_request(float &out_dx, float &out_dy) {
        if (!m_has_pending_move_delta) return false;
        m_has_pending_move_delta = false;
        out_dx = m_pending_move_dx;
        out_dy = m_pending_move_dy;
        m_pending_move_dx = 0.0f;
        m_pending_move_dy = 0.0f;
        return true;
    }

    bool pop_set_layer_offset_request(float &out_x, float &out_y) {
        if (!m_has_pending_abs_offset) return false;
        m_has_pending_abs_offset = false;
        out_x = m_pending_abs_offset_x;
        out_y = m_pending_abs_offset_y;
        return true;
    }

    bool pop_center_layer_request() {
        const bool req = m_req_center_layer;
        m_req_center_layer = false;
        return req;
    }

    bool pop_crop_request(int &out_x, int &out_y, int &out_w, int &out_h) {
        if (!m_req_apply_crop) return false;
        m_req_apply_crop = false;
        out_x = m_pending_crop_rect.x;
        out_y = m_pending_crop_rect.y;
        out_w = m_pending_crop_rect.w;
        out_h = m_pending_crop_rect.h;
        return true;
    }

    void sync_viewport_angle(float angle_deg) {
        m_last_angle = angle_deg;
        if (!m_is_dragging_angle_slider) {
            m_rotate_angle_deg = HorizonAlignState::normalize_angle_signed180(angle_deg);
        }
    }

    void set_active_layer_view(const LayerStatusView &view) { m_active_layer_view = view; }
    const LayerStatusView &get_active_layer_view() const { return m_active_layer_view; }

    void set_checkerboard_config(const CheckerboardConfig &cfg) { m_checkerboard = cfg; }
    const CheckerboardConfig &get_checkerboard_config() const { return m_checkerboard; }

    void set_text_caret_preview(const TextCaretPreview &preview) { m_text_caret_preview = preview; }
    const TextCaretPreview &get_text_caret_preview() const { return m_text_caret_preview; }

    RasterEraser &get_raster_eraser() { return m_raster_eraser; }
    const RasterEraser &get_raster_eraser() const { return m_raster_eraser; }

    CropToolState &get_crop_state() { return m_crop_state; }
    const CropToolState &get_crop_state() const { return m_crop_state; }

    HorizonAlignState &get_horizon_state() { return m_horizon_state; }
    const HorizonAlignState &get_horizon_state() const { return m_horizon_state; }

    void trigger_apply_crop();
    void cancel_crop_tool();

    void set_depth_status_text(const std::string &status, bool is_busy = false) {
        m_depth_status_text = status;
        m_depth_is_busy = is_busy;
    }
    const std::string &get_depth_status_text() const { return m_depth_status_text; }
    bool is_depth_busy() const { return m_depth_is_busy; }
    void set_depth_busy(bool busy) { m_depth_is_busy = busy; }

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
    const std::string &get_inpaint_status_text() const { return m_inpaint_status_text; }
    bool is_inpaint_busy() const { return m_inpaint_is_busy; }
    void set_inpaint_busy(bool busy) { m_inpaint_is_busy = busy; }

    void request_inpaint();
    void on_erase_stroke_finished(bool was_subtract);

    struct SimMouseEvent { float x; float y; int action; };
    void queue_mouse_event(float x, float y, int action) {
        m_sim_mouse_queue.push_back({ x, y, action });
    }

    void register_dom_last_item(const std::string &id, const std::string &label, const std::string &type, bool active = false, bool enabled = true) {
        m_dom.register_item(id, label, type, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), active, enabled);
    }
    const UiDomRegistry &get_dom() const { return m_dom; }
    std::string get_dom_json() const;

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
        const int count = static_cast<int>(m_tabs.size());
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
    void reset_erase_input_state() {
        m_current_stroke.clear();
        m_is_painting_mask = false;
        m_is_right_click_subtract = false;
        m_last_paint_px = ImVec2(-1.0f, -1.0f);
    }

    void clear_pending_doc_requests() {
        m_req_inpaint = false;
        m_req_clear_mask = false;
        m_req_generate_depth = false;
    }

    void set_erase_strokes(const std::vector<std::vector<ImVec2>> &strokes,
                           const std::vector<std::vector<std::vector<ImVec2>>> &undo_stack,
                           const std::vector<std::vector<std::vector<ImVec2>>> &redo_stack) {
        m_erase_strokes = strokes;
        m_erase_undo_stack = undo_stack;
        m_erase_redo_stack = redo_stack;
        reset_erase_input_state();
    }

    void clear_erase_strokes();
    bool undo_erase_stroke();
    bool redo_erase_stroke();
    void push_erase_undo();
    void subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y);
    void sync_erase_strokes_to_tool(uint32_t img_w, uint32_t img_h);

    std::vector<ShaderLabEffect> &get_effects() { return m_effects; }
    const std::vector<ShaderLabEffect> &get_effects() const { return m_effects; }
    void set_effects(const std::vector<ShaderLabEffect> &effects) { m_effects = effects; }
    int get_selected_effect_idx() const { return m_selected_effect_idx; }
    void set_selected_effect_idx(int idx) { m_selected_effect_idx = idx; }

    bool pop_add_shader_request(std::wstring &out_path) {
        if (!m_req_add_shader) return false;
        m_req_add_shader = false;
        out_path = m_add_shader_path;
        return true;
    }
    void simulate_canvas_mouse(float x, float y, int act);

    HWND get_hwnd() const { return m_hwnd; }
    UiDomRegistry &get_dom_mut() { return m_dom; }

    // Public lifecycle / cross-module bridge fields & callbacks (for menu_bar, right_panel, main.cpp)
    float m_right_split_ratio = 0.52f;
    bool m_req_save_project = false;
    bool m_req_save_project_as = false;
    std::wstring m_save_project_path;
    bool m_req_export_image = false;
    bool m_req_export_image_as = false;
    std::wstring m_export_image_path;
    bool m_req_open_prefs = false;
    bool m_show_unsaved_exit_modal = false;
    bool m_unsaved_exit_modal_open = false;
    bool m_req_save_and_exit = false;
    bool m_req_force_exit_without_saving = false;
    CheckerboardConfig m_checkerboard{};

    std::function<void()> on_document_dirty;
    std::function<void()> render_exit_modal_fn;
    std::function<void()> render_layers_panel_fn;
    std::function<void()> render_upscale_tab_fn;
    std::function<void()> render_text_tab_fn;
    std::function<void()> render_prefs_tab_fn;

    std::function<void(float dx, float dy)> on_move_layer_delta;
    std::function<void(float abs_x, float abs_y)> on_set_layer_offset;
    std::function<void()> on_center_layer;
    std::function<void(int x, int y, int w, int h)> on_crop_document;
    std::function<void(float angle_deg, bool constrain_scale)> on_rotate_apply;
    std::function<void(bool horiz, bool vert)> on_flip_layer;
    std::function<LayerPixelView()> on_get_active_layer_pixels;
    std::function<void(int dirty_x, int dirty_y, int dirty_w, int dirty_h)> on_commit_layer_pixels;
    std::function<void(const std::vector<uint8_t> &before_rgba)> on_push_layer_pixel_undo;
    std::function<void(float doc_x, float doc_y)> on_text_canvas_click;

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
    void render_tool_options_bar(float x_offset, float top_offset, float width, float height,
                                 uint32_t img_w, uint32_t img_h);

    void screen_to_doc_px(float sx, float sy, float &out_dx, float &out_dy) const;
    int hit_test_crop_handle(float sx, float sy) const;
    void apply_rotation_delta(float new_angle_deg);
    void nudge_active_layer(float dx, float dy);

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
    bool m_live_depth_peek = false;
    bool m_depth_key_held = false;
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
    bool m_is_right_click_subtract = false;
    std::vector<std::vector<ImVec2>> m_erase_strokes;
    std::vector<ImVec2> m_current_stroke;
    std::vector<std::vector<std::vector<ImVec2>>> m_erase_undo_stack;
    std::vector<std::vector<std::vector<ImVec2>>> m_erase_redo_stack;
    bool m_is_painting_mask = false;
    ImVec2 m_last_paint_px = { -1.0f, -1.0f };

    // Rotate & Horizon state (Feature #6)
    HorizonAlignState m_horizon_state{};
    bool m_is_dragging_angle_slider = false;
    float m_rotate_angle_deg = 0.0f;
    bool m_req_set_viewport_angle = false;
    float m_target_viewport_angle = 0.0f;
    bool m_req_flip_horizontal = false;
    bool m_req_flip_vertical = false;

    // Move Tool state (Feature #8)
    LayerStatusView m_active_layer_view{};
    bool m_show_layer_bounds = true;
    bool m_is_dragging_move = false;
    ImVec2 m_move_drag_last_screen = { 0.0f, 0.0f };
    ImVec2 m_move_drag_accum = { 0.0f, 0.0f };
    int m_move_locked_axis = 0; // 0 = free, 1 = horizontal, 2 = vertical
    bool m_has_pending_move_delta = false;
    float m_pending_move_dx = 0.0f;
    float m_pending_move_dy = 0.0f;
    bool m_has_pending_abs_offset = false;
    float m_pending_abs_offset_x = 0.0f;
    float m_pending_abs_offset_y = 0.0f;
    bool m_req_center_layer = false;

    // Raster Alpha Eraser state (Feature #12)
    RasterEraser m_raster_eraser{};
    bool m_is_raster_erasing = false;
    ImVec2 m_last_raster_erase_px = { -1.0f, -1.0f };

    // Crop Tool state (Feature #13)
    CropToolState m_crop_state{};
    bool m_is_dragging_crop = false;
    ImVec2 m_crop_drag_last_norm = { 0.0f, 0.0f };
    bool m_req_apply_crop = false;
    CropToolState::PixelRect m_pending_crop_rect{};

    // Text Tool preview overlay (Feature #15)
    TextCaretPreview m_text_caret_preview{};

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
    bool m_req_zoom_in = false;
    bool m_req_zoom_out = false;
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
    float m_last_angle = 0.0f;

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
    bool m_show_shortcuts_dialog = false;

    struct CursorHitState {
        float screen_w = 0.0f;
        float viewable_x = 0.0f;
        float viewable_y = 0.0f;
        float viewable_w = 0.0f;
        float viewable_h = 0.0f;
        bool in_canvas = false;
        bool near_left_splitter = false;
        bool near_right_splitter = false;
        bool near_split_line = false;
        bool popup_open = false;
        bool in_client = false;
        int crop_handle = -1;
    };

    bool has_loaded_image() const { return m_last_img_w > 0 && m_last_img_h > 0; }
    bool is_viewport_panning() const { return m_vp_callbacks.is_panning && m_vp_callbacks.is_panning(m_vp_callbacks.user_data); }
    bool is_viewport_capturing() const { return m_vp_callbacks.is_capturing && m_vp_callbacks.is_capturing(m_vp_callbacks.user_data); }
    bool vp_handle_input(UINT msg, WPARAM wParam, LPARAM lParam) const {
        return m_vp_callbacks.handle_input && m_vp_callbacks.handle_input(m_vp_callbacks.user_data, msg, wParam, lParam);
    }
    void vp_cancel_input() const {
        if (m_vp_callbacks.cancel_input) m_vp_callbacks.cancel_input(m_vp_callbacks.user_data);
    }
    void vp_set_space_pan(bool active) const {
        if (m_vp_callbacks.set_space_pan) m_vp_callbacks.set_space_pan(m_vp_callbacks.user_data, active);
    }
    bool has_active_drag() const {
        return m_resizing_left || m_resizing_right || m_is_dragging_split ||
               m_is_painting_mask || m_horizon_state.is_drawing_line ||
               m_is_dragging_move || m_is_raster_erasing || m_is_dragging_crop ||
               is_viewport_capturing();
    }
    void begin_capture();
    void end_capture();
    void cancel_all_drags();
    CursorHitState compute_cursor_hit_state(POINT client_pt) const;
    void apply_cursor_for_state(const CursorHitState &hit);

    HCURSOR m_cursor_grab = nullptr;
    HCURSOR m_cursor_grip = nullptr;
    ViewportCallbacks m_vp_callbacks{};
    POINT m_last_mouse_pt = { 0, 0 };
    bool m_cursor_in_client = false;
    bool m_space_held = false;
    bool m_owns_capture = false;
    std::string m_active_cursor_name = "arrow";
};

} // namespace gui
