#include "cli_info.h"
#include "../../common/depth_chunk.h"
#include "../../common/depth_file.h"
#include "../image_loader.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <sstream>
#include <algorithm>

#include "../../third_party/stb/stb_image.h"

namespace fs = std::filesystem;

#include "../../../common/str_utils.h"

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

int CliInfo::execute(const CliOptions &opts) {
    if (opts.input_path.empty()) {
        std::cerr << "Error: No input image specified for 'info' command.\n";
        std::cerr << "Usage: ShaderLab.exe info <image_path> [--json]\n";
        return 1;
    }

    if (!fs::exists(opts.input_path)) {
        std::cerr << "Error: Input file does not exist: " << wide_to_utf8(opts.input_path.c_str()) << "\n";
        return 1;
    }

    std::string path_u8 = wide_to_utf8(opts.input_path.c_str());
    uintmax_t file_size = fs::file_size(opts.input_path);
    std::string ext = fs::path(opts.input_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)::tolower(c); });

    // read dimensions using stb_image info and make sure it aint corrupted
    int w = 0, h = 0, comp = 0;
    FILE *f = nullptr;
    _wfopen_s(&f, opts.input_path.c_str(), L"rb");
    if (!f || !stbi_info_from_file(f, &w, &h, &comp) || w <= 0 || h <= 0) {
        if (f) fclose(f);
        std::cerr << "Error: File is not a valid or supported image: " << path_u8 << "\n";
        return 1;
    }
    fclose(f);

    bool has_embedded_depth = false;
    DepthMapHeader embed_hdr = {};
    std::vector<float> embed_floats;

    if (ext == ".png") {
        std::ifstream in(opts.input_path, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            size_t size = in.tellg();
            in.seekg(0, std::ios::beg);
            std::vector<uint8_t> png_bytes(size);
            in.read(reinterpret_cast<char*>(png_bytes.data()), size);
            in.close();

            std::string err;
            has_embedded_depth = depth_chunk::extract_sldp(png_bytes.data(), png_bytes.size(), embed_hdr, embed_floats, err);
        }
    }

    bool has_sidecar = false;
    fs::path sidecar_p = opts.input_path;
    sidecar_p.replace_extension(".sldepth");
    std::string sidecar_u8 = wide_to_utf8(sidecar_p.wstring().c_str());

    SidecarDepthHeader sidecar_hdr = {};
    std::vector<float> sidecar_floats;
    if (fs::exists(sidecar_p)) {
        std::string err;
        has_sidecar = depth_file::read_sidecar(sidecar_u8, sidecar_hdr, sidecar_floats, err);
    }

    // compute min/max range if depth present
    float d_min = 0.0f, d_max = 0.0f;
    const std::vector<float> *active_floats = nullptr;
    if (has_embedded_depth && !embed_floats.empty()) {
        active_floats = &embed_floats;
    } else if (has_sidecar && !sidecar_floats.empty()) {
        active_floats = &sidecar_floats;
    }

    if (active_floats && !active_floats->empty()) {
        d_min = *std::min_element(active_floats->begin(), active_floats->end());
        d_max = *std::max_element(active_floats->begin(), active_floats->end());
    }

    if (opts.json_output) {
        std::cout << "{\n"
                  << "  \"path\": \"" << json_escape(path_u8) << "\",\n"
                  << "  \"file_size_bytes\": " << file_size << ",\n"
                  << "  \"width\": " << w << ",\n"
                  << "  \"height\": " << h << ",\n"
                  << "  \"channels\": " << comp << ",\n"
                  << "  \"has_depth\": " << ((has_embedded_depth || has_sidecar) ? "true" : "false") << ",\n"
                  << "  \"depth_type\": \"" << (has_embedded_depth ? "embedded_png_slDp" : has_sidecar ? "companion_sldepth" : "none") << "\",\n";

        if (has_embedded_depth) {
            std::cout << "  \"depth\": {\n"
                      << "    \"source\": \"embedded_png\",\n"
                      << "    \"width\": " << embed_hdr.width << ",\n"
                      << "    \"height\": " << embed_hdr.height << ",\n"
                      << "    \"far_plane\": " << embed_hdr.far_plane << ",\n"
                      << "    \"is_flat\": " << (!(embed_hdr.flags & kDepthFlagValid) ? "true" : "false") << ",\n"
                      << "    \"min_depth\": " << d_min << ",\n"
                      << "    \"max_depth\": " << d_max << "\n"
                      << "  }\n";
        } else if (has_sidecar) {
            std::cout << "  \"depth\": {\n"
                      << "    \"source\": \"companion_sidecar\",\n"
                      << "    \"sidecar_path\": \"" << json_escape(sidecar_u8) << "\",\n"
                      << "    \"width\": " << sidecar_hdr.width << ",\n"
                      << "    \"height\": " << sidecar_hdr.height << ",\n"
                      << "    \"far_plane\": " << sidecar_hdr.far_plane_used << ",\n"
                      << "    \"is_flat\": " << ((sidecar_hdr.flags & kSidecarFlagFlat) ? "true" : "false") << ",\n"
                      << "    \"raw_size_bytes\": " << sidecar_hdr.raw_byte_size << ",\n"
                      << "    \"compressed_size_bytes\": " << sidecar_hdr.compressed_byte_size << ",\n"
                      << "    \"compression_ratio\": " << std::fixed << std::setprecision(2)
                      << (sidecar_hdr.compressed_byte_size > 0 ? (float)sidecar_hdr.raw_byte_size / sidecar_hdr.compressed_byte_size : 0.0f) << ",\n"
                      << "    \"game_name\": \"" << json_escape(sidecar_hdr.game_name) << "\",\n"
                      << "    \"min_depth\": " << d_min << ",\n"
                      << "    \"max_depth\": " << d_max << "\n"
                      << "  }\n";
        } else {
            std::cout << "  \"depth\": null\n";
        }
        std::cout << "}\n";
        return 0;
    }

    // standard terminal output
    std::cout << "========================================\n"
              << "  ShaderLab Image & Depth Inspector\n"
              << "========================================\n"
              << "File:        " << path_u8 << "\n"
              << "Resolution:  " << w << " x " << h << " (" << comp << " channels)\n"
              << "File Size:   " << (file_size / 1024.0) << " KB (" << file_size << " bytes)\n"
              << "Depth State: ";

    if (has_embedded_depth) {
        std::cout << "[EMBEDDED slDp PNG CHUNK]\n"
                  << "  Depth Buffer: " << embed_hdr.width << " x " << embed_hdr.height << " (32-bit Float R32F Deflate)\n"
                  << "  Far Plane:    " << embed_hdr.far_plane << " units\n"
                  << "  Depth Range:  [" << d_min << " .. " << d_max << "]\n"
                  << "  Flat Frame:   " << (!(embed_hdr.flags & kDepthFlagValid) ? "YES (uniform geometry)" : "NO (rich 3D geometry)") << "\n";
    } else if (has_sidecar) {
        float ratio = sidecar_hdr.compressed_byte_size > 0 ? (float)sidecar_hdr.raw_byte_size / sidecar_hdr.compressed_byte_size : 0.0f;
        std::cout << "[COMPANION .sldepth SIDECAR]\n"
                  << "  Sidecar:      " << sidecar_u8 << "\n"
                  << "  Depth Buffer: " << sidecar_hdr.width << " x " << sidecar_hdr.height << " (32-bit Float R32F Deflate)\n"
                  << "  Raw Size:     " << (sidecar_hdr.raw_byte_size / (1024.0 * 1024.0)) << " MB\n"
                  << "  Compressed:   " << (sidecar_hdr.compressed_byte_size / (1024.0 * 1024.0)) << " MB (" << std::fixed << std::setprecision(1) << ratio << "x compression ratio)\n"
                  << "  Far Plane:    " << sidecar_hdr.far_plane_used << " units\n"
                  << "  Game Tag:     " << sidecar_hdr.game_name << "\n"
                  << "  Depth Range:  [" << d_min << " .. " << d_max << "]\n";
    } else {
        std::cout << "[NONE] (2D Image - No depth buffer detected)\n"
                  << "Tip: Use 'ShaderLab.exe depth --input \"" << path_u8 << "\"' to generate a depth buffer.\n";
    }

    std::cout << "========================================\n";
    return 0;
}
