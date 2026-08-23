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
    case WM_SIZE: {
        auto *self = reinterpret_cast<GfxDevice *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
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
        if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH) > 0) {
            auto *self = reinterpret_cast<GfxDevice *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (self) {
                self->set_dropped_file(filePath);
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

bool GfxDevice::initialize(const wchar_t *title, uint32_t win_width, uint32_t win_height, uint32_t init_render_width, uint32_t init_render_height) {
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

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

void GfxDevice::shutdown() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
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
    wc.style = CS_CLASSDC;
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
            return false;
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

    set_render_target();
    return true;
}

bool GfxDevice::resize_buffers(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (m_render_width == width && m_render_height == height && m_rtv) {
        return true;
    }

    m_render_width = width;
    m_render_height = height;

    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_rtv.Reset();

    // tried ResizeBuffers(0, 0, ...) to let dxgi keep the size once, it nuked the rtv and never recovered. dont.
    HRESULT hr = m_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[GfxDevice] ResizeBuffers failed with hr=0x" << std::hex << hr << L"\n";
        return false;
    }

    return create_rtv();
}

void GfxDevice::handle_window_resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_win_width = width;
    m_win_height = height;

    if (!m_is_processing_image && m_swap_chain) {
        resize_buffers(width, height);
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

void GfxDevice::set_dropped_file(const std::wstring &path) {
    m_dropped_file = path;
}

bool GfxDevice::get_and_clear_dropped_file(std::wstring &out_path) {
    if (!m_dropped_file.empty()) {
        out_path = m_dropped_file;
        m_dropped_file.clear();
        return true;
    }
    return false;
}
