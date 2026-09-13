#include "gfx_device.h"
#include <shellapi.h>
#include <iostream>

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = reinterpret_cast<GfxDevice *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self && self->dispatch_input_hook(msg, wParam, lParam)) {
        return 0;
    }

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ENTERSIZEMOVE:
        if (self) self->on_enter_sizemove();
        return 0;
    case WM_EXITSIZEMOVE:
        if (self) self->on_exit_sizemove();
        return 0;
    case WM_SIZE: {
        if (self && wParam != SIZE_MINIMIZED) {
            uint32_t w = static_cast<uint32_t>(LOWORD(lParam));
            uint32_t h = static_cast<uint32_t>(HIWORD(lParam));
            self->handle_window_resize(w, h);
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t filePath[MAX_PATH] = {};
        POINT pt = {};
        DragQueryPoint(hDrop, &pt);
        if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH) > 0) {
            if (self) {
                self->set_dropped_file(filePath, pt.x, pt.y);
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

GfxDevice::GfxDevice() = default;

GfxDevice::~GfxDevice() {
    shutdown();
}

bool GfxDevice::initialize(const wchar_t *title, uint32_t win_width, uint32_t win_height, uint32_t init_render_width, uint32_t init_render_height, bool visible) {
    m_win_width = win_width;
    m_win_height = win_height;
    m_render_width = init_render_width;
    m_render_height = init_render_height;

    if (!create_window(title, m_win_width, m_win_height)) {
        std::wcerr << L"[GfxDevice] Failed to create Win32 window\n";
        return false;
    }

    if (!create_device_and_swap_chain(m_render_width, m_render_height)) {
        std::wcerr << L"[GfxDevice] Failed to create D3D11 device and swap chain\n";
        return false;
    }

    if (visible) {
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        UpdateWindow(m_hwnd);
        SetForegroundWindow(m_hwnd);
        BringWindowToTop(m_hwnd);
    } else {
        ShowWindow(m_hwnd, SW_HIDE);
    }

    m_prev_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(m_hwnd, &m_prev_placement);
    m_prev_style = GetWindowLongW(m_hwnd, GWL_STYLE);
    m_is_fullscreen = false;

    return true;
}

void GfxDevice::shutdown() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_depth_canvas_srv.Reset();
    m_depth_canvas_rtv.Reset();
    m_depth_canvas_tex.Reset();
    m_rtv.Reset();
    m_swap_chain.Reset();
    m_context.Reset();
    m_device.Reset();

    if (m_hwnd) {
        DragAcceptFiles(m_hwnd, FALSE);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(L"ReShadeImagePipelineHostClass", GetModuleHandleW(nullptr));
}

#include "../resource.h"

bool GfxDevice::create_window(const wchar_t *title, uint32_t width, uint32_t height) {
    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"ReShadeImagePipelineHostClass";

    RegisterClassExW(&wc);

    RECT work_area = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int screen_w = work_area.right - work_area.left;
    int screen_h = work_area.bottom - work_area.top;

    int posX = (screen_w > static_cast<int>(width)) ? (work_area.left + (screen_w - static_cast<int>(width)) / 2) : CW_USEDEFAULT;
    int posY = (screen_h > static_cast<int>(height)) ? (work_area.top + (screen_h - static_cast<int>(height)) / 2) : CW_USEDEFAULT;

    RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr
    );

    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        DragAcceptFiles(m_hwnd, TRUE);
        if (hIcon) {
            SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
            SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
        }
    }

    return (m_hwnd != nullptr);
}

bool GfxDevice::create_device_and_swap_chain(uint32_t width, uint32_t height) {
    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL feature_level;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creation_flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        &m_device,
        &feature_level,
        &m_context
    );

    if (FAILED(hr)) {
        creation_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creation_flags,
            feature_levels,
            _countof(feature_levels),
            D3D11_SDK_VERSION,
            &m_device,
            &feature_level,
            &m_context
        );
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                creation_flags,
                feature_levels,
                _countof(feature_levels),
                D3D11_SDK_VERSION,
                &m_device,
                &feature_level,
                &m_context
            );
            if (FAILED(hr)) {
                return false;
            }
        }
    }

    ComPtr<IDXGIDevice2> dxgi_device;
    if (FAILED(m_device.As(&dxgi_device))) return false;

    ComPtr<IDXGIAdapter> dxgi_adapter;
    if (FAILED(dxgi_device->GetAdapter(&dxgi_adapter))) return false;

    ComPtr<IDXGIFactory2> dxgi_factory;
    if (FAILED(dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width  = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling    = DXGI_SCALING_STRETCH; // bb decoupled from window size, this is the whole trick.

    hr = dxgi_factory->CreateSwapChainForHwnd(
        m_device.Get(),
        m_hwnd,
        &desc,
        nullptr,
        nullptr,
        &m_swap_chain
    );

    if (FAILED(hr)) return false;

    dxgi_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    return create_rtv();
}

bool GfxDevice::create_rtv() {
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &m_rtv);
    if (FAILED(hr)) return false;

    if (!create_depth_canvas(m_render_width, m_render_height)) {
        std::wcerr << L"[GfxDevice] Warning: Failed to create depth canvas\n";
    }

    set_render_target();
    return true;
}

bool GfxDevice::create_depth_canvas(uint32_t width, uint32_t height) {
    m_depth_canvas_srv.Reset();
    m_depth_canvas_rtv.Reset();
    m_depth_canvas_tex.Reset();

    if (!m_device || width == 0 || height == 0) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_depth_canvas_tex);
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;

    hr = m_device->CreateRenderTargetView(m_depth_canvas_tex.Get(), &rtv_desc, &m_depth_canvas_rtv);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(m_depth_canvas_tex.Get(), &srv_desc, &m_depth_canvas_srv);
    if (FAILED(hr)) return false;

    clear_depth_canvas(1.0f);
    return true;
}

bool GfxDevice::resize_buffers(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    uint32_t target_w = (std::max)(width, 64u);
    uint32_t target_h = (std::max)(height, 64u);

    if (m_render_width == target_w && m_render_height == target_h && m_rtv) {
        return true;
    }

    m_render_width = target_w;
    m_render_height = target_h;

    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_rtv.Reset();
    m_depth_canvas_srv.Reset();
    m_depth_canvas_rtv.Reset();
    m_depth_canvas_tex.Reset();

    HRESULT hr = m_swap_chain->ResizeBuffers(2, target_w, target_h, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[GfxDevice] ResizeBuffers failed with hr=0x" << std::hex << hr << L"\n";
        return false;
    }

    return create_rtv();
}

void GfxDevice::on_enter_sizemove() {
    m_in_sizemove = true;
    m_resize_pending = false;
}

void GfxDevice::on_exit_sizemove() {
    m_in_sizemove = false;
    if (m_resize_pending && !m_is_processing_image && m_swap_chain) {
        m_resize_pending = false;
        resize_buffers(m_pending_width, m_pending_height);
    }
}

void GfxDevice::handle_window_resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_win_width = width;
    m_win_height = height;

    if (m_in_sizemove) {
        m_resize_pending = true;
        m_pending_width = width;
        m_pending_height = height;
        return;
    }

    if (!m_is_processing_image && m_swap_chain) {
        resize_buffers(width, height);
    }
}

void GfxDevice::toggle_fullscreen() {
    set_fullscreen(!m_is_fullscreen);
}

void GfxDevice::set_fullscreen(bool fullscreen) {
    if (!m_hwnd || m_is_fullscreen == fullscreen) return;

    if (fullscreen) {
        m_prev_placement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(m_hwnd, &m_prev_placement)) {
            return;
        }

        m_prev_style = GetWindowLongW(m_hwnd, GWL_STYLE);

        HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(hMonitor, &mi)) {
            return;
        }

        m_is_fullscreen = true;

        DWORD fs_style = m_prev_style & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE);
        SetWindowLongW(m_hwnd, GWL_STYLE, fs_style | WS_POPUP);

        SetWindowPos(m_hwnd, HWND_TOP,
                     mi.rcMonitor.left,
                     mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        m_is_fullscreen = false;

        DWORD style = m_prev_style ? m_prev_style : WS_OVERLAPPEDWINDOW;
        SetWindowLongW(m_hwnd, GWL_STYLE, style);

        SetWindowPlacement(m_hwnd, &m_prev_placement);

        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void GfxDevice::set_processing_image(bool processing) {
    m_is_processing_image = processing;
    if (!m_is_processing_image && m_swap_chain && m_win_width > 0 && m_win_height > 0) {
        resize_buffers(m_win_width, m_win_height);
    }
}

void GfxDevice::set_render_target() {
    if (!m_context || !m_rtv) return;

    ID3D11RenderTargetView *rtvs[] = { m_rtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, nullptr);
    reset_viewport();
}

void GfxDevice::set_depth_render_target() {
    if (!m_context || !m_depth_canvas_rtv) return;

    ID3D11RenderTargetView *rtvs[] = { m_depth_canvas_rtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, nullptr);
    reset_viewport();
}

void GfxDevice::clear_depth_canvas(float clear_value) {
    if (m_context && m_depth_canvas_rtv) {
        float color[4] = { clear_value, clear_value, clear_value, 1.0f };
        m_context->ClearRenderTargetView(m_depth_canvas_rtv.Get(), color);
    }
}

void GfxDevice::set_viewport_aspect_fit(uint32_t img_width, uint32_t img_height) {
    if (!m_context) return;

    D3D11_VIEWPORT vp = {};
    if (img_width > 0 && img_height > 0 && m_render_width > 0 && m_render_height > 0) {
        float screen_aspect = static_cast<float>(m_render_width) / static_cast<float>(m_render_height);
        float img_aspect = static_cast<float>(img_width) / static_cast<float>(img_height);

        if (img_aspect > screen_aspect) {
            // wide image -> letterbox top/bottom
            vp.Width = static_cast<float>(m_render_width);
            vp.Height = static_cast<float>(m_render_width) / img_aspect;
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = (static_cast<float>(m_render_height) - vp.Height) * 0.5f;
        } else {
            // tall image -> pillarbox left/right
            vp.Width = static_cast<float>(m_render_height) * img_aspect;
            vp.Height = static_cast<float>(m_render_height);
            vp.TopLeftX = (static_cast<float>(m_render_width) - vp.Width) * 0.5f;
            vp.TopLeftY = 0.0f;
        }
    } else {
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(m_render_width);
        vp.Height = static_cast<float>(m_render_height);
    }
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void GfxDevice::reset_viewport() {
    if (!m_context) return;
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(m_render_width);
    vp.Height = static_cast<float>(m_render_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void GfxDevice::clear(const float color[4]) {
    if (m_context && m_rtv) {
        m_context->ClearRenderTargetView(m_rtv.Get(), color);
    }
}

HRESULT GfxDevice::present(UINT sync_interval, UINT flags) {
    if (!m_swap_chain) return E_POINTER;
    return m_swap_chain->Present(sync_interval, flags);
}

void GfxDevice::set_dropped_file(const std::wstring &path, int x, int y) {
    m_dropped_file = path;
    m_dropped_x = x;
    m_dropped_y = y;
}

bool GfxDevice::get_and_clear_dropped_file(std::wstring &out_path, int &out_x, int &out_y) {
    if (!m_dropped_file.empty()) {
        out_path = m_dropped_file;
        out_x = m_dropped_x;
        out_y = m_dropped_y;
        m_dropped_file.clear();
        return true;
    }
    return false;
}

void GfxDevice::add_input_hook(InputHook hook, void *user_data) {
    if (!hook) return;
    m_input_hooks.push_back({ hook, user_data });
}

void GfxDevice::remove_input_hook(InputHook hook, void *user_data) {
    m_input_hooks.erase(
        std::remove_if(m_input_hooks.begin(), m_input_hooks.end(), [&](const HookEntry &e) {
            return e.hook == hook && e.user_data == user_data;
        }),
        m_input_hooks.end()
    );
}

void GfxDevice::clear_input_hooks() {
    m_input_hooks.clear();
}

bool GfxDevice::dispatch_input_hook(UINT msg, WPARAM wParam, LPARAM lParam) {
    for (const auto &entry : m_input_hooks) {
        if (entry.hook && entry.hook(entry.user_data, msg, wParam, lParam)) {
            return true;
        }
    }
    return false;
}
