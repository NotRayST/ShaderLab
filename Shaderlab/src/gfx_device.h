#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

class GfxDevice {
public:
    GfxDevice();
    ~GfxDevice();

    bool initialize(const wchar_t *title, uint32_t win_width, uint32_t win_height, uint32_t init_render_width, uint32_t init_render_height);
    void shutdown();

    bool resize_buffers(uint32_t width, uint32_t height);
    void handle_window_resize(uint32_t width, uint32_t height);
    void set_processing_image(bool processing);
    bool is_processing_image() const { return m_is_processing_image; }

    void set_render_target();
    void set_viewport_aspect_fit(uint32_t img_width, uint32_t img_height);
    void reset_viewport();
    void clear(const float color[4]);
    HRESULT present(UINT sync_interval = 1, UINT flags = 0);

    bool get_and_clear_dropped_file(std::wstring &out_path);
    void set_dropped_file(const std::wstring &path);

    using InputHook = bool(*)(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam);
    void set_input_hook(InputHook hook, void *user_data) { m_input_hook = hook; m_input_hook_user = user_data; }
    bool dispatch_input_hook(UINT msg, WPARAM wParam, LPARAM lParam) {
        if (m_input_hook) return m_input_hook(m_input_hook_user, msg, wParam, lParam);
        return false;
    }

    HWND get_hwnd() const { return m_hwnd; }
    ID3D11Device *get_device() const { return m_device.Get(); }
    ID3D11DeviceContext *get_context() const { return m_context.Get(); }
    IDXGISwapChain1 *get_swap_chain() const { return m_swap_chain.Get(); }
    ID3D11RenderTargetView *get_rtv() const { return m_rtv.Get(); }
    uint32_t get_render_width() const { return m_render_width; }
    uint32_t get_render_height() const { return m_render_height; }
    uint32_t get_win_width() const { return m_win_width; }
    uint32_t get_win_height() const { return m_win_height; }

private:
    bool create_window(const wchar_t *title, uint32_t width, uint32_t height);
    bool create_device_and_swap_chain(uint32_t width, uint32_t height);
    bool create_rtv();

    HWND m_hwnd = nullptr;
    uint32_t m_win_width = 1280;
    uint32_t m_win_height = 720;
    uint32_t m_render_width = 1280;
    uint32_t m_render_height = 720;
    bool m_is_processing_image = false;

    std::wstring m_dropped_file;
    InputHook m_input_hook = nullptr;
    void *m_input_hook_user = nullptr;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swap_chain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
};
