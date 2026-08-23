#include "backbuffer_dump.h"
#include "image_loader.h"
#include <iostream>
#include <vector>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

static std::string wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

bool BackbufferDump::capture_to_file(GfxDevice &gfx, const wchar_t *output_path, uint32_t width, uint32_t height) {
    if (!gfx.get_device() || !gfx.get_context() || !gfx.get_swap_chain() || !output_path) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = gfx.get_device()->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to create staging texture\n";
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    hr = gfx.get_swap_chain()->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to get swap chain back buffer\n";
        return false;
    }

    gfx.get_context()->CopyResource(staging.Get(), back_buffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = gfx.get_context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to map staging texture\n";
        return false;
    }

    // make sure the out dir exists before writing (output_path may point into a subfolder)
    std::filesystem::path p(output_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::string out_utf8 = wide_to_utf8(output_path);
    int ok = stbi_write_png(
        out_utf8.c_str(),
        static_cast<int>(width),
        static_cast<int>(height),
        4,
        mapped.pData,
        static_cast<int>(mapped.RowPitch)
    );

    gfx.get_context()->Unmap(staging.Get(), 0);

    return (ok != 0);
}

bool BackbufferDump::run_selftest(GfxDevice &gfx, BlitRenderer &blit, const wchar_t *input_path, const wchar_t *output_path) {
    std::wcout << L"[SelfTest] Loading input image: " << input_path << L"\n";

    LoadedImage img;
    if (!ImageLoader::load_from_file(gfx.get_device(), input_path, img)) {
        std::wcerr << L"[SelfTest] Image load failed: " << img.error_msg << L"\n";
        return false;
    }

    std::wcout << L"[SelfTest] Image loaded: " << img.width << L"x" << img.height << L" pixels\n";

    if (!gfx.resize_buffers(img.width, img.height)) {
        std::wcerr << L"[SelfTest] Failed to resize swap chain back buffer to native dimensions\n";
        return false;
    }

    std::wcout << L"[SelfTest] Decoupled swap chain resized: back-buffer is " << gfx.get_render_width()
               << L"x" << gfx.get_render_height() << L" (window remains fixed size)\n";

    // present a fixed batch of frames so the pipeline is stable. god knows why 30, but whatever
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < 30; ++i) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        gfx.set_render_target();
        gfx.clear(clear_color);
        blit.render(gfx.get_context(), img.srv.Get());
        gfx.present(1, 0);
    }

    std::wstring final_out = output_path ? output_path : L"_selftest.png";
    std::wcout << L"[SelfTest] Capturing back buffer to: " << final_out << L"\n";

    bool success = capture_to_file(gfx, final_out.c_str(), img.width, img.height);
    if (success) {
        std::wcout << L"[SelfTest] SUCCESS: Dumped " << img.width << L"x" << img.height << L" PNG to " << final_out << L"\n";
    } else {
        std::wcerr << L"[SelfTest] FAILED to write PNG output\n";
    }

    return success;
}
