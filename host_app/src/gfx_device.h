#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class GfxDevice {
public:
    GfxDevice();
    ~GfxDevice();

    bool initialize(const wchar_t *title, uint32_t win_width, uint32_t win_height, uint32_t init_render_width, uint32_t init_render_height, bool visible = true);
    void shutdown();

    bool resize_buffers(uint32_t width, uint32_t height);
    void handle_window_resize(uint32_t width, uint32_t height);
    void on_enter_sizemove();
    void on_exit_sizemove();
    void set_processing_image(bool processing);
    bool is_processing_image() const { return m_is_processing_image; }

    void set_render_target();
    void set_depth_render_target();
    void set_viewport_aspect_fit(uint32_t img_width, uint32_t img_height);
    void reset_viewport();
    void clear(const float color[4]);
    void clear_depth_canvas(float clear_value = 1.0f);
    HRESULT present(UINT sync_interval = 1, UINT flags = 0);

    bool get_and_clear_dropped_file(std::wstring &out_path, int &out_x, int &out_y);
    void set_dropped_file(const std::wstring &path, int x, int y);

    using InputHook = bool(*)(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam);
    void add_input_hook(InputHook hook, void *user_data);
    void remove_input_hook(InputHook hook, void *user_data);
    void clear_input_hooks();


    bool dispatch_input_hook(UINT msg, WPARAM wParam, LPARAM lParam);

    bool is_fullscreen() const { return m_is_fullscreen; }
    void toggle_fullscreen();
    void set_fullscreen(bool fullscreen);

    HWND get_hwnd() const { return m_hwnd; }
    ID3D11Device *get_device() const { return m_device.Get(); }
    ID3D11DeviceContext *get_context() const { return m_context.Get(); }
    IDXGISwapChain1 *get_swap_chain() const { return m_swap_chain.Get(); }
    ID3D11RenderTargetView *get_rtv() const { return m_rtv.Get(); }
    ID3D11RenderTargetView *get_depth_canvas_rtv() const { return m_depth_canvas_rtv.Get(); }
    ID3D11ShaderResourceView *get_depth_canvas_srv() const { return m_depth_canvas_srv.Get(); }
    uint32_t get_render_width() const { return m_render_width; }
    uint32_t get_render_height() const { return m_render_height; }
    uint32_t get_win_width() const { return m_win_width; }
    uint32_t get_win_height() const { return m_win_height; }

private:
    bool create_window(const wchar_t *title, uint32_t width, uint32_t height);
    bool create_device_and_swap_chain(uint32_t width, uint32_t height);
    bool create_rtv();
    bool create_depth_canvas(uint32_t width, uint32_t height);

    HWND m_hwnd = nullptr;
    uint32_t m_win_width = 1280;
    uint32_t m_win_height = 720;
    uint32_t m_render_width = 1280;
    uint32_t m_render_height = 720;
    bool m_is_processing_image = false;
    bool m_is_fullscreen = false;
    bool m_in_sizemove = false;
    bool m_resize_pending = false;
    uint32_t m_pending_width = 0;
    uint32_t m_pending_height = 0;
    WINDOWPLACEMENT m_prev_placement = { sizeof(WINDOWPLACEMENT) };
    DWORD m_prev_style = 0;

    std::wstring m_dropped_file;
    int m_dropped_x = 0;
    int m_dropped_y = 0;

    struct HookEntry {
        InputHook hook;
        void *user_data;
    };
    std::vector<HookEntry> m_input_hooks;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swap_chain;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    // 3D Depth Canvas
    ComPtr<ID3D11Texture2D> m_depth_canvas_tex;
    ComPtr<ID3D11RenderTargetView> m_depth_canvas_rtv;
    ComPtr<ID3D11ShaderResourceView> m_depth_canvas_srv;
};
