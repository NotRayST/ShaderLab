#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

struct LoadedImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t error_code = 0;
    std::wstring error_msg;
};

class ImageLoader {
public:
    static bool load_from_file(ID3D11Device *device, const wchar_t *path, LoadedImage &out_image);
};
