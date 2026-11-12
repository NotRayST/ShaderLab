#include "backbuffer_dump.h"
#include "image_loader.h"
#include <iostream>
#include <vector>
#include <filesystem>

#include "../../common/fast_png_zlib.h"
#include "../../common/project_file.h"
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

static bool write_mapped_rgba_to_file(const uint8_t *src_bytes, UINT row_pitch, uint32_t width, uint32_t height, bool is_bgra, const wchar_t *output_path) {
    std::filesystem::path p(output_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::string out_utf8 = wide_to_utf8(output_path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    std::vector<uint8_t> packed(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row_src = src_bytes + (static_cast<size_t>(y) * row_pitch);
        uint8_t *row_dst = packed.data() + (static_cast<size_t>(y) * width * 4);
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t c0 = row_src[x * 4 + 0];
            uint8_t c1 = row_src[x * 4 + 1];
            uint8_t c2 = row_src[x * 4 + 2];
            row_dst[x * 4 + 0] = is_bgra ? c2 : c0;
            row_dst[x * 4 + 1] = c1;
            row_dst[x * 4 + 2] = is_bgra ? c0 : c2;
            row_dst[x * 4 + 3] = 255;
        }
    }

    int ok = 0;
    if (ext == ".jpg" || ext == ".jpeg") {
        ok = stbi_write_jpg(out_utf8.c_str(), static_cast<int>(width), static_cast<int>(height), 4, packed.data(), 95);
    } else {
        stbi_write_force_png_filter = 0;
        ok = stbi_write_png(
            out_utf8.c_str(),
            static_cast<int>(width),
            static_cast<int>(height),
            4,
            packed.data(),
            static_cast<int>(width * 4)
        );
    }
    return (ok != 0);
}

bool BackbufferDump::save_texture_to_file(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *src_tex, const wchar_t *output_path) {
    if (!device || !context || !src_tex || !output_path) {
        return false;
    }

    D3D11_TEXTURE2D_DESC src_desc = {};
    src_tex->GetDesc(&src_desc);
    if (src_desc.Width == 0 || src_desc.Height == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = src_desc.Width;
    staging_desc.Height = src_desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = src_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] save_texture_to_file: failed to create staging texture (hr=0x" << std::hex << hr << std::dec << L")\n";
        return false;
    }

    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src_tex, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] save_texture_to_file: failed to map staging texture\n";
        return false;
    }

    bool is_bgra = (src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    bool ok = write_mapped_rgba_to_file(static_cast<const uint8_t *>(mapped.pData), mapped.RowPitch, src_desc.Width, src_desc.Height, is_bgra, output_path);
    context->Unmap(staging.Get(), 0);
    return ok;
}

bool BackbufferDump::capture_to_file(GfxDevice &gfx, const wchar_t *output_path, uint32_t width, uint32_t height) {
    if (!gfx.get_device() || !gfx.get_context() || !gfx.get_swap_chain() || !output_path) {
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = gfx.get_swap_chain()->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to get swap chain back buffer\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC bb_desc = {};
    back_buffer->GetDesc(&bb_desc);
    uint32_t copy_w = (std::min)(width, bb_desc.Width);
    uint32_t copy_h = (std::min)(height, bb_desc.Height);
    if (copy_w == 0 || copy_h == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = copy_w;
    desc.Height = copy_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = bb_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    hr = gfx.get_device()->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to create staging texture\n";
        return false;
    }

    D3D11_BOX src_box = { 0, 0, 0, copy_w, copy_h, 1 };
    gfx.get_context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, back_buffer.Get(), 0, &src_box);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = gfx.get_context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::wcerr << L"[BackbufferDump] Failed to map staging texture\n";
        return false;
    }

    bool is_bgra = (bb_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bb_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    bool ok = write_mapped_rgba_to_file(static_cast<const uint8_t *>(mapped.pData), mapped.RowPitch, copy_w, copy_h, is_bgra, output_path);
    gfx.get_context()->Unmap(staging.Get(), 0);

    return ok;
}

bool BackbufferDump::run_selftest(GfxDevice &gfx, BlitRenderer &blit, const wchar_t *input_path, const wchar_t *output_path) {
    LoadedImage img;
    bool loaded = false;
    if (input_path && *input_path) {
        if (project_file::is_project_file(input_path)) {
            std::cout << "[SelfTest] Input is a ShaderLab project: " << wide_to_utf8(input_path) << "\n";
            wchar_t temp_dir[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp_dir);
            std::filesystem::path ws = std::filesystem::path(temp_dir) / "ShaderLab" / "workspace";
            std::wstring out_img, out_preset, err;
            project_file::ProjectManifest manifest;
            if (project_file::load_project(input_path, ws.wstring(), out_img, out_preset, manifest, err)) {
                std::cout << "[SelfTest] Project unpacked: image=" << wide_to_utf8(out_img) << ", preset=" << wide_to_utf8(out_preset) << "\n";
                loaded = ImageLoader::load_from_file(gfx.get_device(), out_img.c_str(), img);
            } else {
                std::cout << "[SelfTest] Project unpack failed: " << wide_to_utf8(err) << "\n";
            }
        } else {
            std::cout << "[SelfTest] Loading input image: " << wide_to_utf8(input_path) << "\n";
            loaded = ImageLoader::load_from_file(gfx.get_device(), input_path, img);
        }
        if (!loaded) {
            std::cout << "[SelfTest] Image load failed: " << wide_to_utf8(img.error_msg) << " (using procedural test pattern)\n";
        }
    }

    if (!loaded) {
        img.width = 256;
        img.height = 256;
        std::vector<uint32_t> test_pixels(img.width * img.height);
        for (uint32_t y = 0; y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                uint8_t r = static_cast<uint8_t>(x);
                uint8_t g = static_cast<uint8_t>(y);
                uint8_t b = static_cast<uint8_t>((x + y) / 2);
                test_pixels[y * img.width + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
            }
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = img.width;
        desc.Height = img.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = test_pixels.data();
        init_data.SysMemPitch = img.width * sizeof(uint32_t);

        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(gfx.get_device()->CreateTexture2D(&desc, &init_data, &tex))) {
            gfx.get_device()->CreateShaderResourceView(tex.Get(), nullptr, &img.srv);
            loaded = true;
            std::cout << "[SelfTest] Generated procedural test pattern: " << img.width << "x" << img.height << "\n";
        } else {
            std::cerr << "[SelfTest] Failed to create procedural test texture\n";
            return false;
        }
    }

    std::cout << "[SelfTest] Test surface ready: " << img.width << "x" << img.height << " pixels\n";

    if (!gfx.resize_buffers(img.width, img.height)) {
        std::cerr << "[SelfTest] Failed to resize swap chain back buffer to native dimensions\n";
        return false;
    }

    std::cout << "[SelfTest] Decoupled swap chain resized: back-buffer is " << gfx.get_render_width()
              << "x" << gfx.get_render_height() << " (window remains fixed size)\n";

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
        gfx.present(0, 0);
    }

    std::wstring final_out = output_path ? output_path : L"_selftest.png";
    std::cout << "[SelfTest] Capturing back buffer to: " << wide_to_utf8(final_out) << "\n";

    bool success = capture_to_file(gfx, final_out.c_str(), img.width, img.height);
    if (success) {
        std::cout << "[SelfTest] SUCCESS: Dumped " << img.width << "x" << img.height << " PNG to " << wide_to_utf8(final_out) << "\n";
    } else {
        std::cerr << "[SelfTest] FAILED to write PNG output\n";
    }

    return success;
}
