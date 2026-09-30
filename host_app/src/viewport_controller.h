#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "../../common/ipc_protocol.h"

struct ViewportTransform {
    float angle = 0.0f;     // degrees clockwise, 0 = upright
    float zoom  = 1.0f;     // >1 = magnified (crop), range 0.25..32.0
    float pan_x = 0.0f;     // image-space offset of view center from image center in source pixels
    float pan_y = 0.0f;

    bool is_identity() const {
        return (std::abs(angle) < 0.001f || std::abs(angle - 360.0f) < 0.001f) &&
               (std::abs(zoom - 1.0f) < 0.001f) &&
               (std::abs(pan_x) < 0.001f) &&
               (std::abs(pan_y) < 0.001f);
    }
};

class ViewportController {
public:
    ViewportController() = default;
    ~ViewportController() = default;

    void reset();
    void reset_zoom_and_pan();
    void reset_rotation();

    bool handle_input(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, uint32_t render_w, uint32_t render_h, uint32_t img_w, uint32_t img_h);

    const ViewportTransform &get_transform() const { return m_transform; }
    float get_snapped_angle() const;
    float get_raw_angle() const { return m_raw_angle; }
    bool is_snapped() const { return m_is_snapped; }
    bool is_fine() const { return m_is_fine; }
    bool is_rotating() const { return m_is_rotating; }
    bool is_panning() const { return m_is_panning; }

    void set_locked(bool locked) { m_is_locked = locked; }
    bool is_locked() const { return m_is_locked; }

    void set_fine_vk(uint32_t vk) { m_fine_vk = vk; }

    void sync_to_block(SharedControlBlock *block);
    void sync_from_block(const SharedControlBlock *block);

    // computes the rotated bounding box dimensions for the export canvas
    static void get_rotated_aabb(uint32_t in_w, uint32_t in_h, float angle_deg, uint32_t &out_w, uint32_t &out_h);

    // helpers to get the screen-space image center and image dims on screen
    void get_screen_image_geometry(
        uint32_t render_w, uint32_t render_h,
        uint32_t img_w, uint32_t img_h,
        float &out_cx, float &out_cy,
        float &out_screen_w, float &out_screen_h
    ) const;

    bool screen_to_image_uv(
        float screen_x, float screen_y,
        uint32_t render_w, uint32_t render_h,
        uint32_t img_w, uint32_t img_h,
        float &out_u, float &out_v
    ) const;

    void set_lock_zoom(bool l) { m_lock_zoom = l; }
    bool is_lock_zoom() const { return m_lock_zoom; }
    void set_lock_rotate(bool l) { m_lock_rotate = l; }
    bool is_lock_rotate() const { return m_lock_rotate; }
    void set_lock_pan(bool l) { m_lock_pan = l; }
    bool is_lock_pan() const { return m_lock_pan; }

    bool is_pan_locked_attempt() const { return m_is_pan_locked_attempt; }
    bool is_rotate_locked_attempt() const { return m_is_rotate_locked_attempt; }

    bool matches_keybind(IpcAction action, WPARAM vk, bool ctrl, bool shift, bool alt) const;

    bool is_before_after_enabled() const { return m_before_after_enabled; }
    float get_before_after_angle() const { return m_before_after_angle; }
    float get_before_after_split() const { return m_before_after_split; }
    bool is_dragging_split_pos() const { return m_is_dragging_split_pos; }
    bool is_dragging_split_rot() const { return m_is_dragging_split_rot; }
    void set_before_after_enabled(bool enabled);
    void toggle_before_after() { set_before_after_enabled(!m_before_after_enabled); }
    bool is_erase_active() const { return m_erase_active; }
    void set_erase_active(bool e) { m_erase_active = e; }

private:
    void update_snap();
    void clamp_pan(uint32_t render_w, uint32_t render_h, uint32_t img_w, uint32_t img_h);

    ViewportTransform m_transform;
    bool m_is_locked = false;
    bool m_erase_active = false;
    bool m_lock_zoom = false;
    bool m_lock_rotate = false;
    bool m_lock_pan = false;

    // interaction states
    bool m_is_panning = false;
    bool m_is_rotating = false;
    bool m_is_snapped = false;
    bool m_is_fine = false;

    bool m_is_pan_locked_attempt = false;
    bool m_is_rotate_locked_attempt = false;

    // before/after comparison state
    bool  m_before_after_enabled = false;
    float m_before_after_angle = 0.0f;
    float m_before_after_split = 0.0f;
    bool  m_is_dragging_split_pos = false;
    bool  m_is_dragging_split_rot = false;
    bool  m_is_hovering_split = false;
    bool  m_split_changed = false;
    bool  m_angle_changed = false;
    float m_raw_split_angle = 0.0f;
    bool  m_saved_lock_zoom = false;
    bool  m_saved_lock_rotate = false;
    bool  m_saved_lock_pan = false;

    IpcKeybind m_keybinds[kMaxKeybinds] = {};
    uint32_t   m_keybind_version = 0;

    POINT m_last_mouse_client = { 0, 0 };
    float m_last_drag_angle = 0.0f;
    float m_raw_angle = 0.0f;
    float m_wheel_accum = 0.0f;
    uint32_t m_fine_vk = VK_SHIFT;
    uint64_t m_last_lock_ms = 0;
};
