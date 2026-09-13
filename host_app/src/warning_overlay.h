#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include "hud_renderer.h"

class WarningOverlay : public GdiHudLayer {
public:
    WarningOverlay();
    ~WarningOverlay() override;

    bool initialize(ID3D11Device *device) override;
    void shutdown() override;

    bool is_active() const { return m_active && !m_dismissed; }
    void set_active(bool active);
    void dismiss() { m_dismissed = true; }

    void on_mouse_move(int x, int y);
    bool on_mouse_click(int x, int y);
    void on_key_down(WPARAM key);

    void update_and_render(ID3D11DeviceContext *context, uint32_t width, uint32_t height, float dt);

private:
    void render_gdiplus(ID3D11DeviceContext *context, uint32_t width, uint32_t height);

    bool m_active = false;
    bool m_dismissed = false;
    bool m_sound_played = false;

    int m_mouse_x = 0;
    int m_mouse_y = 0;
    int m_hovered_button = -1; // 0 = Download, 1 = Dismiss

    RECT m_btn_download = {};
    RECT m_btn_dismiss  = {};
};
