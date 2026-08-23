#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class WarningOverlay {
public:
    WarningOverlay();
    ~WarningOverlay();

    bool initialize(ID3D11Device *device);
    void shutdown();

    bool is_active() const { return m_active && !m_dismissed; }
    void set_active(bool active);
    void dismiss() { m_dismissed = true; }

    void on_mouse_move(int x, int y);
    bool on_mouse_click(int x, int y);
    void on_key_down(WPARAM key);

    void update_and_render(ID3D11DeviceContext *context, uint32_t width, uint32_t height, float dt);
    ID3D11ShaderResourceView *get_srv() const { return m_srv.Get(); }

private:
    void render_gdiplus(uint32_t width, uint32_t height);

    ID3D11Device *m_device = nullptr;
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11ShaderResourceView> m_srv;

    ULONG_PTR m_gdiplus_token = 0;
    uint32_t m_tex_width = 0;
    uint32_t m_tex_height = 0;

    bool m_active = false;
    bool m_dismissed = false;
    bool m_sound_played = false;
    float m_fade_alpha = 0.0f;

    int m_mouse_x = 0;
    int m_mouse_y = 0;
    int m_hovered_button = -1; // 0 = Download, 1 = Dismiss

    RECT m_btn_download = {};
    RECT m_btn_dismiss  = {};
};
