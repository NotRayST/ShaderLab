#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct LoadedImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t error_code = 0;
    std::wstring error_msg;

    // 3D Depth support
    ComPtr<ID3D11Texture2D> depth_texture;
    ComPtr<ID3D11ShaderResourceView> depth_srv;
    std::vector<float> depth_pixels;
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
    bool has_depth = false;
    float far_plane = 1000.0f;
    bool is_flat = false;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

class ImageLoader {
public:
    static bool load_from_file(ID3D11Device *device, const wchar_t *path, LoadedImage &out_image);
};
