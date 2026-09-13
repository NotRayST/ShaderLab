#include "viewport_controller.h"
#include <windowsx.h>
#include <cctype>

static constexpr float kPi = 3.14159265358979323846f;

static void init_default_keybinds(IpcKeybind *kb) {
    kb[static_cast<uint32_t>(IpcAction::Undo)]          = { 'Z', 1, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::Redo)]          = { 'Y', 1, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::ResetRotation)] = { 'R', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::ResetZoomPan)]  = { 'Q', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::NudgeLeft)]     = { VK_LEFT, 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::NudgeRight)]    = { VK_RIGHT, 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::FineTune)]      = { VK_SHIFT, 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::LockPan)]       = { 'H', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::LockZoom)]      = { 'J', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::LockRotate)]    = { 'K', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::LockView)]      = { 'L', 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::ToggleFullscreen)] = { VK_F11, 0, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::SaveProject)]   = { 'S', 1, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::SaveProjectAs)] = { 'S', 1, 1, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::ExportImage)]   = { 'E', 1, 0, 0, 0 };
    kb[static_cast<uint32_t>(IpcAction::ExportImageAs)] = { 'E', 1, 1, 0, 0 };
}

bool ViewportController::matches_keybind(IpcAction action, WPARAM vk, bool ctrl, bool shift, bool alt) const {
    uint32_t idx = static_cast<uint32_t>(action);
    if (idx >= kMaxKeybinds) return false;
    IpcKeybind kb = m_keybinds[idx];
    if (m_keybind_version == 0 || kb.vk == 0) {
        IpcKeybind def[kMaxKeybinds] = {};
        init_default_keybinds(def);
        kb = def[idx];
    }
    if (kb.vk == 0) return false;

    WPARAM target_vk = kb.vk;
    bool key_matched = false;

    if (vk >= 'A' && vk <= 'Z') {
        key_matched = (vk == static_cast<WPARAM>(std::toupper(static_cast<int>(target_vk))));
    } else if (vk >= '0' && vk <= '9') {
        key_matched = (vk == target_vk || (target_vk == '0' && vk == VK_NUMPAD0));
    } else if (target_vk == VK_SHIFT || target_vk == VK_LSHIFT || target_vk == VK_RSHIFT) {
        key_matched = (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT);
    } else if (target_vk == VK_CONTROL || target_vk == VK_LCONTROL || target_vk == VK_RCONTROL) {
        key_matched = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL);
    } else if (target_vk == VK_MENU || target_vk == VK_LMENU || target_vk == VK_RMENU) {
        key_matched = (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
    } else {
        key_matched = (vk == target_vk);
    }

    if (!key_matched) return false;

    bool is_target_ctrl  = (target_vk == VK_CONTROL || target_vk == VK_LCONTROL || target_vk == VK_RCONTROL);
    bool is_target_shift = (target_vk == VK_SHIFT   || target_vk == VK_LSHIFT   || target_vk == VK_RSHIFT);
    bool is_target_alt   = (target_vk == VK_MENU    || target_vk == VK_LMENU    || target_vk == VK_RMENU);

    if (!is_target_ctrl  && ((kb.ctrl != 0)  != ctrl))  return false;
    if (!is_target_shift && ((kb.shift != 0) != shift)) return false;
    if (!is_target_alt   && ((kb.alt != 0)   != alt))   return false;

    return true;
}

void ViewportController::reset() {
    m_transform = ViewportTransform{};
    m_raw_angle = 0.0f;
    m_is_snapped = false;
    m_is_fine = false;
    m_is_panning = false;
    m_is_rotating = false;
    m_wheel_accum = 0.0f;
}

void ViewportController::reset_zoom_and_pan() {
    m_transform.zoom = 1.0f;
    m_transform.pan_x = 0.0f;
    m_transform.pan_y = 0.0f;
}

void ViewportController::reset_rotation() {
    m_transform.angle = 0.0f;
    m_raw_angle = 0.0f;
    m_is_snapped = false;
    m_is_fine = false;
}

float ViewportController::get_snapped_angle() const {
    return m_transform.angle;
}

void ViewportController::get_rotated_aabb(uint32_t in_w, uint32_t in_h, float angle_deg, uint32_t &out_w, uint32_t &out_h) {
    if (in_w == 0 || in_h == 0) {
        out_w = (std::max)(1u, in_w);
        out_h = (std::max)(1u, in_h);
        return;
    }

    float rad = std::abs(angle_deg) * (kPi / 180.0f);
    float cos_a = std::abs(std::cos(rad));
    float sin_a = std::abs(std::sin(rad));

    float normalized_deg = std::fmod(std::abs(angle_deg), 360.0f);
    if (std::abs(normalized_deg - 0.0f) < 0.01f || std::abs(normalized_deg - 180.0f) < 0.01f) {
        out_w = in_w;
        out_h = in_h;
        return;
    }
    if (std::abs(normalized_deg - 90.0f) < 0.01f || std::abs(normalized_deg - 270.0f) < 0.01f) {
        out_w = in_h;
        out_h = in_w;
        return;
    }

    float w_prime = std::round(static_cast<float>(in_w) * cos_a + static_cast<float>(in_h) * sin_a);
    float h_prime = std::round(static_cast<float>(in_w) * sin_a + static_cast<float>(in_h) * cos_a);

    out_w = static_cast<uint32_t>(std::clamp(w_prime, 1.0f, 16384.0f));
    out_h = static_cast<uint32_t>(std::clamp(h_prime, 1.0f, 16384.0f));
}

void ViewportController::get_screen_image_geometry(
    uint32_t render_w, uint32_t render_h,
    uint32_t img_w, uint32_t img_h,
    float &out_cx, float &out_cy,
    float &out_screen_w, float &out_screen_h
) const {
    out_cx = static_cast<float>(render_w) * 0.5f;
    out_cy = static_cast<float>(render_h) * 0.5f;

    if (img_w == 0 || img_h == 0 || render_w == 0 || render_h == 0) {
        out_screen_w = static_cast<float>(render_w);
        out_screen_h = static_cast<float>(render_h);
        return;
    }

    float screen_aspect = static_cast<float>(render_w) / static_cast<float>(render_h);
    float img_aspect = static_cast<float>(img_w) / static_cast<float>(img_h);

    if (img_aspect > screen_aspect) {
        out_screen_w = static_cast<float>(render_w) * m_transform.zoom;
        out_screen_h = (static_cast<float>(render_w) / img_aspect) * m_transform.zoom;
    } else {
        out_screen_w = (static_cast<float>(render_h) * img_aspect) * m_transform.zoom;
        out_screen_h = static_cast<float>(render_h) * m_transform.zoom;
    }
}

bool ViewportController::screen_to_image_uv(
    float screen_x, float screen_y,
    uint32_t render_w, uint32_t render_h,
    uint32_t img_w, uint32_t img_h,
    float &out_u, float &out_v
) const {
    float cx = 0.0f, cy = 0.0f, sw = 0.0f, sh = 0.0f;
    get_screen_image_geometry(render_w, render_h, img_w, img_h, cx, cy, sw, sh);
    if (sw <= 0.0f || sh <= 0.0f) return false;

    float center_x = cx + m_transform.pan_x;
    float center_y = cy + m_transform.pan_y;

    float rel_x = (screen_x - center_x) / sw + 0.5f;
    float rel_y = (screen_y - center_y) / sh + 0.5f;

    out_u = std::clamp(rel_x, 0.0f, 1.0f);
    out_v = std::clamp(rel_y, 0.0f, 1.0f);
    return (rel_x >= 0.0f && rel_x <= 1.0f && rel_y >= 0.0f && rel_y <= 1.0f);
}

void ViewportController::update_snap() {
    float norm = std::fmod(m_raw_angle, 360.0f);
    if (norm < 0.0f) norm += 360.0f;

    if (m_is_fine) {
        m_transform.angle = norm;
        m_is_snapped = false;
        return;
    }

    float nearest_snap = std::round(norm / 45.0f) * 45.0f;
    float diff = std::abs(norm - nearest_snap);
    if (diff > 180.0f) diff = 360.0f - diff;

    if (diff <= 4.0f) {
        if (nearest_snap >= 360.0f) nearest_snap -= 360.0f;
        m_transform.angle = nearest_snap;
        m_is_snapped = true;
    } else {
        m_transform.angle = norm;
        m_is_snapped = false;
    }
}

void ViewportController::clamp_pan(uint32_t /*render_w*/, uint32_t /*render_h*/, uint32_t img_w, uint32_t img_h) {
    if (img_w == 0 || img_h == 0) return;

    float max_pan_x = static_cast<float>(img_w) * 2.0f;
    float max_pan_y = static_cast<float>(img_h) * 2.0f;
    m_transform.pan_x = std::clamp(m_transform.pan_x, -max_pan_x, max_pan_x);
    m_transform.pan_y = std::clamp(m_transform.pan_y, -max_pan_y, max_pan_y);
}

bool ViewportController::handle_input(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    uint32_t render_w, uint32_t render_h,
    uint32_t img_w, uint32_t img_h
) {
    if (m_is_locked) {
        if (m_is_panning || m_is_rotating) {
            ReleaseCapture();
            m_is_panning = false;
            m_is_rotating = false;
        }
        return false;
    }

    RECT client_rect = {};
    GetClientRect(hwnd, &client_rect);
    float client_w = static_cast<float>(client_rect.right - client_rect.left);
    float client_h = static_cast<float>(client_rect.bottom - client_rect.top);
    if (client_w <= 0.0f) client_w = static_cast<float>(render_w);
    if (client_h <= 0.0f) client_h = static_cast<float>(render_h);

    float scale_x = static_cast<float>(render_w) / client_w;
    float scale_y = static_cast<float>(render_h) / client_h;

    float center_x = static_cast<float>(render_w) * 0.5f;
    float center_y = static_cast<float>(render_h) * 0.5f;

    bool shift_down = (GetAsyncKeyState(m_fine_vk) & 0x8000) != 0;
    bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    m_is_fine = shift_down;

    switch (msg) {
    case WM_MOUSEWHEEL: {
        if (m_lock_zoom) return false;
        short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
        POINT pt_screen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt_screen);

        float cur_render_x = static_cast<float>(pt_screen.x) * scale_x;
        float cur_render_y = static_cast<float>(pt_screen.y) * scale_y;

        m_wheel_accum += static_cast<float>(wheel_delta) / 120.0f;
        float notches = std::trunc(m_wheel_accum);
        if (std::abs(notches) >= 1.0f) {
            m_wheel_accum -= notches;

            float zoom_mult = std::pow(m_is_fine ? 1.01f : 1.15f, notches);
            float old_zoom = m_transform.zoom;
            float new_zoom = std::clamp(old_zoom * zoom_mult, 0.25f, 32.0f);

            if (std::abs(new_zoom - old_zoom) > 0.0001f) {
                // zoom about the cursor position
                float p_cur_x = cur_render_x - center_x;
                float p_cur_y = cur_render_y - center_y;

                float rad = m_transform.angle * (kPi / 180.0f);
                float cos_a = std::cos(rad);
                float sin_a = std::sin(rad);

                float rot_x = p_cur_x * cos_a + p_cur_y * sin_a;
                float rot_y = -p_cur_x * sin_a + p_cur_y * cos_a;

                float factor = (1.0f / old_zoom) - (1.0f / new_zoom);
                m_transform.pan_x += rot_x * factor;
                m_transform.pan_y += rot_y * factor;

                m_transform.zoom = new_zoom;
                clamp_pan(render_w, render_h, img_w, img_h);
            }
        }
        return true;
    }

    case WM_LBUTTONDOWN: {
        int client_x = GET_X_LPARAM(lParam);
        int client_y = GET_Y_LPARAM(lParam);
        m_last_mouse_client = { client_x, client_y };

        if (alt_down) {
            if (m_lock_rotate) {
                m_is_rotate_locked_attempt = true;
                SetCapture(hwnd);
                return true;
            }
            m_is_rotate_locked_attempt = false;
            m_is_rotating = true;
            m_is_panning = false;
            SetCapture(hwnd);

            float cur_render_x = static_cast<float>(client_x) * scale_x;
            float cur_render_y = static_cast<float>(client_y) * scale_y;
            float dx = cur_render_x - center_x;
            float dy = cur_render_y - center_y;
            m_last_drag_angle = std::atan2(dx, -dy) * (180.0f / kPi);
            if (m_last_drag_angle < 0.0f) m_last_drag_angle += 360.0f;
            return true;
        } else {
            if (m_lock_pan) {
                m_is_pan_locked_attempt = true;
                SetCapture(hwnd);
                return true;
            }
            m_is_pan_locked_attempt = false;
            m_is_panning = true;
            m_is_rotating = false;
            SetCapture(hwnd);
            return true;
        }
    }

    case WM_RBUTTONDOWN: {
        if (m_lock_rotate) {
            m_is_rotate_locked_attempt = true;
            SetCapture(hwnd);
            return true;
        }
        m_is_rotate_locked_attempt = false;
        int client_x = GET_X_LPARAM(lParam);
        int client_y = GET_Y_LPARAM(lParam);
        m_last_mouse_client = { client_x, client_y };

        m_is_rotating = true;
        m_is_panning = false;
        SetCapture(hwnd);

        float cur_render_x = static_cast<float>(client_x) * scale_x;
        float cur_render_y = static_cast<float>(client_y) * scale_y;
        float dx = cur_render_x - center_x;
        float dy = cur_render_y - center_y;
        m_last_drag_angle = std::atan2(dx, -dy) * (180.0f / kPi);
        if (m_last_drag_angle < 0.0f) m_last_drag_angle += 360.0f;
        return true;
    }

    case WM_MOUSEMOVE: {
        int client_x = GET_X_LPARAM(lParam);
        int client_y = GET_Y_LPARAM(lParam);

        if (m_is_rotating) {
            float cur_render_x = static_cast<float>(client_x) * scale_x;
            float cur_render_y = static_cast<float>(client_y) * scale_y;
            float last_render_x = static_cast<float>(m_last_mouse_client.x) * scale_x;
            float last_render_y = static_cast<float>(m_last_mouse_client.y) * scale_y;

            float d_mx = cur_render_x - last_render_x;
            float d_my = cur_render_y - last_render_y;

            float dx = cur_render_x - center_x;
            float dy = cur_render_y - center_y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq > 25.0f) {
                float dist = std::sqrt(dist_sq);
                float tx = -dy / dist;
                float ty = dx / dist;
                float tang_dist = d_mx * tx + d_my * ty;

                float eff_r = std::clamp(dist, 50.0f, 130.0f);
                float delta_angle = (tang_dist / eff_r) * (180.0f / kPi);

                if (m_is_fine) delta_angle *= 0.25f;

                m_raw_angle += delta_angle;
                update_snap();
            }
            m_last_mouse_client = { client_x, client_y };
            return true;
        }

        if (m_is_panning) {
            float delta_client_x = static_cast<float>(client_x - m_last_mouse_client.x);
            float delta_client_y = static_cast<float>(client_y - m_last_mouse_client.y);
            m_last_mouse_client = { client_x, client_y };

            float dx = delta_client_x * scale_x;
            float dy = delta_client_y * scale_y;

            float rad = m_transform.angle * (kPi / 180.0f);
            float cos_a = std::cos(rad);
            float sin_a = std::sin(rad);

            float rot_dx = dx * cos_a + dy * sin_a;
            float rot_dy = -dx * sin_a + dy * cos_a;

            if (m_is_fine) {
                rot_dx *= 0.25f;
                rot_dy *= 0.25f;
            }

            m_transform.pan_x -= rot_dx / m_transform.zoom;
            m_transform.pan_y -= rot_dy / m_transform.zoom;

            clamp_pan(render_w, render_h, img_w, img_h);
            return true;
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (m_is_pan_locked_attempt || m_is_rotate_locked_attempt) {
            m_is_pan_locked_attempt = false;
            m_is_rotate_locked_attempt = false;
            ReleaseCapture();
            return true;
        }
        if (m_is_panning || m_is_rotating) {
            m_is_panning = false;
            m_is_rotating = false;
            ReleaseCapture();
            return true;
        }
        break;
    }

    case WM_RBUTTONUP: {
        if (m_is_rotate_locked_attempt) {
            m_is_rotate_locked_attempt = false;
            ReleaseCapture();
            return true;
        }
        if (m_is_rotating) {
            m_is_rotating = false;
            ReleaseCapture();
            return true;
        }
        break;
    }

    case WM_LBUTTONDBLCLK: {
        reset();
        return true;
    }

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        bool was_down   = (lParam & (1 << 30)) != 0;
        bool ctrl_down  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift_down_local = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt_down_local   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        if (matches_keybind(IpcAction::ResetRotation, wParam, ctrl_down, shift_down_local, alt_down_local)) {
            reset_rotation();
            return true;
        } else if (matches_keybind(IpcAction::ResetZoomPan, wParam, ctrl_down, shift_down_local, alt_down_local)) {
            reset_zoom_and_pan();
            return true;
        } else if (matches_keybind(IpcAction::NudgeLeft, wParam, ctrl_down, shift_down_local, alt_down_local)) {
            if (m_lock_pan) return false;
            float screen_step = shift_down_local ? 2.0f : 15.0f;
            float rad = m_transform.angle * (kPi / 180.0f);
            float cos_a = std::cos(rad);
            float sin_a = std::sin(rad);
            float rot_dx = (-screen_step) * cos_a;
            float rot_dy = -(-screen_step) * sin_a;
            m_transform.pan_x -= rot_dx / m_transform.zoom;
            m_transform.pan_y -= rot_dy / m_transform.zoom;
            clamp_pan(render_w, render_h, img_w, img_h);
            return true;
        } else if (matches_keybind(IpcAction::NudgeRight, wParam, ctrl_down, shift_down_local, alt_down_local)) {
            if (m_lock_pan) return false;
            float screen_step = shift_down_local ? 2.0f : 15.0f;
            float rad = m_transform.angle * (kPi / 180.0f);
            float cos_a = std::cos(rad);
            float sin_a = std::sin(rad);
            float rot_dx = screen_step * cos_a;
            float rot_dy = -screen_step * sin_a;
            m_transform.pan_x -= rot_dx / m_transform.zoom;
            m_transform.pan_y -= rot_dy / m_transform.zoom;
            clamp_pan(render_w, render_h, img_w, img_h);
            return true;
        }

        uint64_t now_lock_ms = GetTickCount64();
        if (!was_down && (now_lock_ms - m_last_lock_ms >= 200)) {
            if (matches_keybind(IpcAction::LockPan, wParam, ctrl_down, shift_down_local, alt_down_local)) {
                m_lock_pan = !m_lock_pan;
                if (m_lock_pan && m_is_panning) {
                    m_is_panning = false;
                    ReleaseCapture();
                }
                m_last_lock_ms = now_lock_ms;
                return true;
            } else if (matches_keybind(IpcAction::LockZoom, wParam, ctrl_down, shift_down_local, alt_down_local)) {
                m_lock_zoom = !m_lock_zoom;
                m_last_lock_ms = now_lock_ms;
                return true;
            } else if (matches_keybind(IpcAction::LockRotate, wParam, ctrl_down, shift_down_local, alt_down_local)) {
                m_lock_rotate = !m_lock_rotate;
                if (m_lock_rotate && m_is_rotating) {
                    m_is_rotating = false;
                    ReleaseCapture();
                }
                m_last_lock_ms = now_lock_ms;
                return true;
            } else if (matches_keybind(IpcAction::LockView, wParam, ctrl_down, shift_down_local, alt_down_local)) {
                bool any_unlocked = !m_lock_zoom || !m_lock_rotate || !m_lock_pan;
                m_lock_zoom = any_unlocked;
                m_lock_rotate = any_unlocked;
                m_lock_pan = any_unlocked;
                if (m_lock_rotate && m_is_rotating) {
                    m_is_rotating = false;
                    ReleaseCapture();
                }
                if (m_lock_pan && m_is_panning) {
                    m_is_panning = false;
                    ReleaseCapture();
                }
                m_last_lock_ms = now_lock_ms;
                return true;
            }
        }
        break;
    }
    }

    return false;
}

void ViewportController::sync_to_block(SharedControlBlock *block) {
    if (!block) return;
    block->view_angle = m_transform.angle;
    block->view_raw_angle = m_raw_angle;
    block->view_zoom = m_transform.zoom;
    block->view_pan[0] = static_cast<int32_t>(std::round(m_transform.pan_x));
    block->view_pan[1] = static_cast<int32_t>(std::round(m_transform.pan_y));

    uint32_t flags = 0;
    if (m_is_rotating) flags |= VIEW_FLAG_ROTATING;
    if (m_is_panning)  flags |= VIEW_FLAG_PANNING;
    if (m_is_snapped)  flags |= VIEW_FLAG_SNAPPED;
    if (m_is_fine)     flags |= VIEW_FLAG_FINE;
    if (m_lock_zoom)   flags |= VIEW_FLAG_LOCK_ZOOM;
    if (m_lock_rotate) flags |= VIEW_FLAG_LOCK_ROT;
    if (m_lock_pan)    flags |= VIEW_FLAG_LOCK_PAN;
    if (m_is_pan_locked_attempt) flags |= VIEW_FLAG_PAN_LOCKED_ATTEMPT;
    // preserve flags owned outside viewport controller like depth peek and text input
    uint32_t preserved = block->view_interaction_flags & (VIEW_FLAG_DEPTH_PEEK | VIEW_FLAG_TEXT_INPUT);
    block->view_interaction_flags = flags | preserved;
    block->view_last_lock_ms = m_last_lock_ms;
    block->view_transform_version++;
}

void ViewportController::sync_from_block(const SharedControlBlock *block) {
    if (!block) return;
    m_transform.angle = block->view_angle;
    m_raw_angle = block->view_raw_angle;
    m_transform.zoom = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
    m_transform.pan_x = static_cast<float>(block->view_pan[0]);
    m_transform.pan_y = static_cast<float>(block->view_pan[1]);

    m_is_rotating = (block->view_interaction_flags & VIEW_FLAG_ROTATING) != 0;
    m_is_panning  = (block->view_interaction_flags & VIEW_FLAG_PANNING) != 0;
    m_is_snapped  = (block->view_interaction_flags & VIEW_FLAG_SNAPPED) != 0;
    m_is_fine     = (block->view_interaction_flags & VIEW_FLAG_FINE) != 0;
    m_lock_zoom   = (block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0;
    m_lock_rotate = (block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0;
    m_lock_pan    = (block->view_interaction_flags & VIEW_FLAG_LOCK_PAN) != 0;

    if (block->view_last_lock_ms > m_last_lock_ms) {
        m_last_lock_ms = block->view_last_lock_ms;
    }

    if (block->keybind_version != m_keybind_version && block->keybind_version > 0) {
        memcpy(m_keybinds, block->keybind_table, sizeof(m_keybinds));
        m_keybind_version = block->keybind_version;
        m_fine_vk = block->fine_tune_vk;
    }
}

