#include "image_loader.h"
#include "../../common/ipc_protocol.h"
#include <vector>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

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
        // TODO: stbi sometimes returns garbage dims for weird files, should validate header instead
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

    return true;
}
