#include "image_loader.h"
#include "../../common/ipc_protocol.h"
#include "../../common/depth_file.h"
#include "../../common/depth_chunk.h"
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

namespace fs = std::filesystem;

bool ImageLoader::load_from_file(ID3D11Device *device, const wchar_t *path, LoadedImage &out_image) {
    out_image = {};

    if (!device || !path || !*path) {
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"Invalid device or empty file path";
        return false;
    }

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        out_image.error_code = IPC_ERR_FILE_NOT_FOUND;
        out_image.error_msg = L"Failed to open file: " + std::wstring(path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"File is empty: " + std::wstring(path);
        return false;
    }

    std::vector<uint8_t> buffer(file_size);
    size_t read_bytes = fread(buffer.data(), 1, file_size, f);
    fclose(f);

    if (read_bytes != static_cast<size_t>(file_size)) {
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"Failed to read complete file content";
        return false;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels = stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &w, &h, &comp, 4);
    if (!pixels) {
        out_image.error_code = IPC_ERR_UNSUPPORTED_FMT;
        const char *reason = stbi_failure_reason();
        std::string s_reason = reason ? reason : "unknown";
        out_image.error_msg = L"stb_image failed: " + std::wstring(s_reason.begin(), s_reason.end());
        return false;
    }

    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"Invalid image dimensions";
        return false;
    }

    if (w > 16384 || h > 16384) {
        stbi_image_free(pixels);
        out_image.error_code = IPC_ERR_IMAGE_TOO_LARGE;
        out_image.error_msg = L"Image dimensions (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L") exceed D3D11 limit (16384)";
        return false;
    }

    out_image.width = static_cast<uint32_t>(w);
    out_image.height = static_cast<uint32_t>(h);

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = out_image.width;
    tex_desc.Height = out_image.height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA sub_data = {};
    sub_data.pSysMem = pixels;
    sub_data.SysMemPitch = out_image.width * 4;
    sub_data.SysMemSlicePitch = 0;

    HRESULT hr = device->CreateTexture2D(&tex_desc, &sub_data, &out_image.texture);
    stbi_image_free(pixels);

    if (FAILED(hr)) {
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"Failed to create D3D11Texture2D (hr=0x" + std::to_wstring(hr) + L")";
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(out_image.texture.Get(), &srv_desc, &out_image.srv);
    if (FAILED(hr)) {
        out_image.error_code = IPC_ERR_LOAD_FAILED;
        out_image.error_msg = L"Failed to create D3D11ShaderResourceView";
        return false;
    }

    // try sidecar first
    fs::path img_path(path);
    fs::path sidecar_path = img_path;
    sidecar_path.replace_extension(L".sldepth");

    std::vector<float> linear_floats;
    uint32_t depth_w = 0, depth_h = 0;
    float far_plane = 1000.0f;
    bool is_flat = false;
    bool depth_loaded = false;

    if (fs::exists(sidecar_path)) {
        SidecarDepthHeader header = {};
        std::string err;
        int u8_len = WideCharToMultiByte(CP_UTF8, 0, sidecar_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string sidecar_u8(u8_len > 1 ? u8_len - 1 : 0, '\0');
        if (u8_len > 1) {
            WideCharToMultiByte(CP_UTF8, 0, sidecar_path.c_str(), -1, sidecar_u8.data(), u8_len, nullptr, nullptr);
        }

        if (depth_file::read_sidecar(sidecar_u8, header, linear_floats, err)) {
            depth_w = header.width;
            depth_h = header.height;
            far_plane = (header.far_plane_used > 0.0f) ? header.far_plane_used : 1000.0f;
            is_flat = (header.flags & kSidecarFlagFlat) != 0;
            depth_loaded = true;
            std::wcerr << L"[ImageLoader] Loaded .sldepth sidecar (" << depth_w << L"x" << depth_h << L", F=" << far_plane << L")\n";
        } else {
            std::wcerr << L"[ImageLoader] Warning: failed to read .sldepth: " << std::wstring(err.begin(), err.end()) << L"\n";
        }
    }

    // no sidecar, check for embedded slDp chunk
    if (!depth_loaded && buffer.size() > 8) {
        DepthMapHeader chunk_hdr = {};
        std::string chunk_err;
        if (depth_chunk::extract_sldp(buffer.data(), buffer.size(), chunk_hdr, linear_floats, chunk_err)) {
            depth_w = chunk_hdr.width;
            depth_h = chunk_hdr.height;
            far_plane = (chunk_hdr.far_plane > 0.0f) ? chunk_hdr.far_plane : 1000.0f;
            depth_loaded = true;
            std::wcerr << L"[ImageLoader] Loaded embedded slDp PNG chunk (" << depth_w << L"x" << depth_h << L", F=" << far_plane << L")\n";
        }
    }

    // last resort: google pixel / android camera depth embedded in jpeg xmp
    std::string_view s((char*)buffer.data(), buffer.size());
    size_t dp = s.find("Semantic=\"Depth"), ls = (dp != s.npos) ? s.rfind("Length=\"", dp) : s.npos;
    if (!depth_loaded && ls != s.npos) {
        size_t dlen = std::stoull(std::string(s.substr(ls + 8, 16))), after = 0, p = s.find("/>", dp);
        while ((p = s.find("Length=\"", p)) < s.find("</x:xmpmeta>", dp)) { after += std::stoull(std::string(s.substr(p + 8, 16))); p += 8; }
        int dw, dh;
        if (stbi_uc *px = stbi_load_from_memory(buffer.data() + buffer.size() - after - dlen, (int)dlen, &dw, &dh, 0, 1)) {
            linear_floats.assign(px, px + (depth_w = dw) * (depth_h = dh));
            for (float &v : linear_floats) v /= 255.0f;
            depth_loaded = true; stbi_image_free(px);
        }
    }

    if (depth_loaded && depth_w > 0 && depth_h > 0 && linear_floats.size() == depth_w * depth_h) {
        float min_d = 1.0f, max_d = 0.0f;
        depth_file::sanitize_and_analyze(linear_floats.data(), linear_floats.size(), is_flat, min_d, max_d);

        D3D11_TEXTURE2D_DESC dtex_desc = {};
        dtex_desc.Width = depth_w;
        dtex_desc.Height = depth_h;
        dtex_desc.MipLevels = 1;
        dtex_desc.ArraySize = 1;
        dtex_desc.Format = DXGI_FORMAT_R32_FLOAT;
        dtex_desc.SampleDesc.Count = 1;
        dtex_desc.SampleDesc.Quality = 0;
        dtex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        dtex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dtex_desc.CPUAccessFlags = 0;

        D3D11_SUBRESOURCE_DATA dsub_data = {};
        dsub_data.pSysMem = linear_floats.data();
        dsub_data.SysMemPitch = depth_w * sizeof(float);
        dsub_data.SysMemSlicePitch = 0;

        HRESULT dhr = device->CreateTexture2D(&dtex_desc, &dsub_data, &out_image.depth_texture);
        if (SUCCEEDED(dhr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC dsrv_desc = {};
            dsrv_desc.Format = DXGI_FORMAT_R32_FLOAT;
            dsrv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            dsrv_desc.Texture2D.MostDetailedMip = 0;
            dsrv_desc.Texture2D.MipLevels = 1;

            dhr = device->CreateShaderResourceView(out_image.depth_texture.Get(), &dsrv_desc, &out_image.depth_srv);
            if (SUCCEEDED(dhr)) {
                out_image.has_depth = true;
                out_image.far_plane = far_plane;
                out_image.is_flat = is_flat;
                out_image.min_depth = min_d;
                out_image.max_depth = max_d;
                out_image.depth_width = depth_w;
                out_image.depth_height = depth_h;
                out_image.depth_pixels = std::move(linear_floats);
            }
        }
    }

    return true;
}
