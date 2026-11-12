#include "project_lifecycle_ops.h"
#include "str_utils.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace lifecycle_ops {

namespace {

std::string trim_copy(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::wstring to_lower_wcopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

std::string section_name_for_effect(const gui::ShaderLabEffect &fx) {
    if (!fx.file_path.empty()) {
        std::string fname = fs::path(fx.file_path).filename().string();
        if (!fname.empty()) return fname;
    }
    return fx.name.empty() ? std::string("Effect") : fx.name;
}

std::vector<float> parse_csv_floats(const std::string &val) {
    std::vector<float> out;
    std::stringstream ss(val);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try {
            out.push_back(std::stof(trim_copy(tok)));
        } catch (...) {
            out.push_back(0.0f);
        }
    }
    return out;
}

void stb_vec_write_cb(void *context, void *data, int size) {
    if (!context || !data || size <= 0) return;
    auto *vec = static_cast<std::vector<uint8_t> *>(context);
    const auto *bytes = static_cast<const uint8_t *>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

void stb_stream_write_cb(void *context, void *data, int size) {
    if (!context || !data || size <= 0) return;
    auto *out = static_cast<std::ofstream *>(context);
    out->write(reinterpret_cast<const char *>(data), size);
}

struct SectionEntries {
    std::vector<std::pair<std::string, std::string>> ordered;
    std::unordered_map<std::string, std::string> lookup;
};

void apply_section_values_to_effect(
    gui::ShaderLabEffect &fx,
    const SectionEntries &sec,
    bool create_missing_uniforms
) {
    for (auto &u : fx.uniforms) {
        auto it = sec.lookup.find(u.name);
        if (it == sec.lookup.end()) continue;
        const std::string &v = it->second;
        switch (u.type) {
        case gui::ShaderLabUniform::Type::Float:
            try { u.float_val = std::stof(v); } catch (...) {}
            break;
        case gui::ShaderLabUniform::Type::Int:
            try { u.int_val = std::stoi(v); } catch (...) {}
            break;
        case gui::ShaderLabUniform::Type::Bool: {
            std::string low = to_lower_copy(v);
            u.bool_val = (low == "1" || low == "true" || low == "yes");
            break;
        }
        case gui::ShaderLabUniform::Type::Color3:
        case gui::ShaderLabUniform::Type::Color4: {
            auto comps = parse_csv_floats(v);
            for (size_t c = 0; c < comps.size() && c < 4; ++c) {
                u.color_val[c] = comps[c];
            }
            break;
        }
        case gui::ShaderLabUniform::Type::Enum:
            try { u.enum_selected = std::stoi(v); } catch (...) {}
            break;
        }
    }

    if (create_missing_uniforms) {
        for (const auto &kv : sec.ordered) {
            const bool already_exists = std::any_of(fx.uniforms.begin(), fx.uniforms.end(), [&](const gui::ShaderLabUniform &u) {
                return u.name == kv.first;
            });
            if (already_exists) continue;

            gui::ShaderLabUniform u;
            u.name = kv.first;
            u.label = kv.first;
            auto comps = parse_csv_floats(kv.second);
            if (comps.size() >= 3) {
                u.type = (comps.size() == 3) ? gui::ShaderLabUniform::Type::Color3 : gui::ShaderLabUniform::Type::Color4;
                for (size_t c = 0; c < comps.size() && c < 4; ++c) {
                    u.color_val[c] = comps[c];
                    u.default_color[c] = comps[c];
                }
            } else {
                u.type = gui::ShaderLabUniform::Type::Float;
                try { u.float_val = std::stof(kv.second); } catch (...) { u.float_val = 0.0f; }
                u.default_float = u.float_val;
            }
            fx.uniforms.push_back(std::move(u));
        }
    }
}

bool is_bgra_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool is_rgba8_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           is_bgra_format(fmt);
}

} // namespace

std::vector<uint8_t> serialize_effects_to_preset_ini(const std::vector<gui::ShaderLabEffect> &effects) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6);

    std::string enabled_techs;
    std::string sorting_techs;
    for (size_t i = 0; i < effects.size(); ++i) {
        const auto &fx = effects[i];
        const std::string sec = section_name_for_effect(fx);
        const std::string item = (fx.name.empty() ? sec : fx.name) + "@" + sec;
        if (!sorting_techs.empty()) sorting_techs += ",";
        sorting_techs += item;
        if (fx.enabled) {
            if (!enabled_techs.empty()) enabled_techs += ",";
            enabled_techs += item;
        }
    }

    ss << "Techniques=" << enabled_techs << "\n";
    ss << "TechniqueSorting=" << sorting_techs << "\n\n";

    // ShaderLab-specific ordered stack metadata
    ss << "[ShaderLab.Effects]\n";
    ss << "count=" << effects.size() << "\n";
    for (size_t i = 0; i < effects.size(); ++i) {
        const auto &fx = effects[i];
        ss << "fx." << i << ".name=" << fx.name << "\n";
        ss << "fx." << i << ".file_path=" << fx.file_path << "\n";
        ss << "fx." << i << ".engine_type=" << fx.engine_type << "\n";
        ss << "fx." << i << ".enabled=" << (fx.enabled ? 1 : 0) << "\n";
    }
    ss << "\n";

    for (const auto &fx : effects) {
        ss << "[" << section_name_for_effect(fx) << "]\n";
        for (const auto &u : fx.uniforms) {
            ss << u.name << "=";
            switch (u.type) {
            case gui::ShaderLabUniform::Type::Float:
                ss << u.float_val;
                break;
            case gui::ShaderLabUniform::Type::Int:
                ss << u.int_val;
                break;
            case gui::ShaderLabUniform::Type::Bool:
                ss << (u.bool_val ? 1 : 0);
                break;
            case gui::ShaderLabUniform::Type::Color3:
                ss << u.color_val[0] << "," << u.color_val[1] << "," << u.color_val[2];
                break;
            case gui::ShaderLabUniform::Type::Color4:
                ss << u.color_val[0] << "," << u.color_val[1] << "," << u.color_val[2] << "," << u.color_val[3];
                break;
            case gui::ShaderLabUniform::Type::Enum:
                ss << u.enum_selected;
                break;
            }
            ss << "\n";
        }
        ss << "\n";
    }

    const std::string text = ss.str();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool deserialize_effects_from_preset_ini(const std::vector<uint8_t> &ini_bytes, std::vector<gui::ShaderLabEffect> &effects) {
    if (ini_bytes.empty()) return false;

    std::string text(ini_bytes.begin(), ini_bytes.end());
    std::istringstream iss(text);

    std::unordered_map<std::string, SectionEntries> sections;
    std::string global_techniques;
    std::string current_section;
    std::string line;

    while (std::getline(iss, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(trimmed.substr(0, eq));
        std::string val = trim_copy(trimmed.substr(eq + 1));

        if (current_section.empty()) {
            if (key == "Techniques") {
                global_techniques = val;
            }
        } else {
            auto &sec = sections[current_section];
            if (!sec.lookup.count(key)) {
                sec.ordered.emplace_back(key, val);
            }
            sec.lookup[key] = val;
        }
    }

    auto meta_it = sections.find("ShaderLab.Effects");
    if (meta_it != sections.end() && meta_it->second.lookup.count("count")) {
        int count = 0;
        try { count = std::max(0, std::stoi(meta_it->second.lookup.at("count"))); } catch (...) { count = 0; }

        std::vector<gui::ShaderLabEffect> rebuilt;
        rebuilt.reserve(static_cast<size_t>(count));

        for (int i = 0; i < count; ++i) {
            std::string prefix = "fx." + std::to_string(i) + ".";
            const auto &m = meta_it->second.lookup;
            std::string name = m.count(prefix + "name") ? m.at(prefix + "name") : "";
            std::string path = m.count(prefix + "file_path") ? m.at(prefix + "file_path") : "";
            std::string eng = m.count(prefix + "engine_type") ? m.at(prefix + "engine_type") : "ReShade FX";
            bool enabled = m.count(prefix + "enabled") && (m.at(prefix + "enabled") != "0");

            gui::ShaderLabEffect entry;
            bool matched_existing = false;
            for (const auto &existing : effects) {
                if ((!path.empty() && existing.file_path == path) || (!name.empty() && existing.name == name)) {
                    entry = existing;
                    matched_existing = true;
                    break;
                }
            }
            if (!matched_existing) {
                entry.name = name;
                entry.file_path = path;
                entry.engine_type = eng;
            }
            entry.enabled = enabled;

            const std::string sec = section_name_for_effect(entry);
            auto sec_it = sections.find(sec);
            if (sec_it != sections.end()) {
                apply_section_values_to_effect(entry, sec_it->second, !matched_existing);
            }
            rebuilt.push_back(std::move(entry));
        }

        effects = std::move(rebuilt);
        return true;
    }

    // Fallback: vanilla ReShade .ini format ("Techniques=Tech@File.fx,...")
    std::vector<std::string> enabled_tokens;
    {
        std::stringstream ts(global_techniques);
        std::string tok;
        while (std::getline(ts, tok, ',')) {
            tok = trim_copy(tok);
            if (!tok.empty()) enabled_tokens.push_back(tok);
        }
    }

    for (auto &fx : effects) {
        const std::string sec = section_name_for_effect(fx);
        const std::string low_sec = to_lower_copy(sec);
        const std::string low_name = to_lower_copy(fx.name);

        if (!global_techniques.empty() || !enabled_tokens.empty()) {
            bool is_on = false;
            for (const auto &tok : enabled_tokens) {
                size_t at_pos = tok.find('@');
                std::string t_name = to_lower_copy(at_pos == std::string::npos ? tok : tok.substr(0, at_pos));
                std::string t_file = to_lower_copy(at_pos == std::string::npos ? "" : tok.substr(at_pos + 1));
                if ((!t_file.empty() && t_file == low_sec) || (!t_name.empty() && t_name == low_name)) {
                    is_on = true;
                    break;
                }
            }
            fx.enabled = is_on;
        }

        auto sec_it = sections.find(sec);
        if (sec_it != sections.end()) {
            apply_section_values_to_effect(fx, sec_it->second, false);
        }
    }

    return true;
}

bool load_preset_ini_file(const std::wstring &ini_path, std::vector<gui::ShaderLabEffect> &effects) {
    if (ini_path.empty()) return false;
    std::ifstream in(fs::path(ini_path), std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char *>(bytes.data()), sz)) return false;
    return deserialize_effects_from_preset_ini(bytes, effects);
}

bool encode_rgba_to_png_bytes(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out_png_bytes) {
    out_png_bytes.clear();
    if (!rgba || width == 0 || height == 0) return false;
    stbi_write_force_png_filter = 0;
    int ok = stbi_write_png_to_func(
        stb_vec_write_cb,
        &out_png_bytes,
        static_cast<int>(width),
        static_cast<int>(height),
        4,
        rgba,
        static_cast<int>(width * 4)
    );
    return ok != 0 && !out_png_bytes.empty();
}

bool decode_png_bytes_to_rgba(const std::vector<uint8_t> &png_bytes, std::vector<uint8_t> &out_rgba, uint32_t &out_w, uint32_t &out_h) {
    out_rgba.clear();
    out_w = 0;
    out_h = 0;
    if (png_bytes.empty()) return false;

    int w = 0, h = 0, comp = 0;
    unsigned char *decoded = stbi_load_from_memory(
        png_bytes.data(), static_cast<int>(png_bytes.size()), &w, &h, &comp, 4
    );
    if (!decoded || w <= 0 || h <= 0) {
        if (decoded) stbi_image_free(decoded);
        return false;
    }

    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h) * 4U;
    out_rgba.assign(decoded, decoded + total);
    stbi_image_free(decoded);
    out_w = static_cast<uint32_t>(w);
    out_h = static_cast<uint32_t>(h);
    return true;
}

bool readback_texture_rgba(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *tex,
    std::vector<uint8_t> &out_rgba,
    uint32_t &out_w,
    uint32_t &out_h,
    bool force_opaque_alpha
) {
    out_rgba.clear();
    out_w = 0;
    out_h = 0;
    if (!device || !context || !tex) return false;

    D3D11_TEXTURE2D_DESC src_desc = {};
    tex->GetDesc(&src_desc);
    if (src_desc.Width == 0 || src_desc.Height == 0 || !is_rgba8_format(src_desc.Format)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = src_desc.Width;
    staging_desc.Height = src_desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = src_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging)) || !staging) {
        return false;
    }

    context->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const bool bgra = is_bgra_format(src_desc.Format);
    const uint32_t w = src_desc.Width;
    const uint32_t h = src_desc.Height;
    out_rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);

    const auto *src_base = static_cast<const uint8_t *>(mapped.pData);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row_src = src_base + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t *row_dst = out_rgba.data() + static_cast<size_t>(y) * w * 4U;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t c0 = row_src[x * 4 + 0];
            const uint8_t c1 = row_src[x * 4 + 1];
            const uint8_t c2 = row_src[x * 4 + 2];
            const uint8_t c3 = row_src[x * 4 + 3];
            row_dst[x * 4 + 0] = bgra ? c2 : c0;
            row_dst[x * 4 + 1] = c1;
            row_dst[x * 4 + 2] = bgra ? c0 : c2;
            row_dst[x * 4 + 3] = force_opaque_alpha ? 255 : c3;
        }
    }

    context->Unmap(staging.Get(), 0);
    out_w = w;
    out_h = h;
    return true;
}

std::wstring ensure_project_extension(const std::wstring &path) {
    if (path.empty() || project_file::is_project_file(path)) return path;
    return path + L".shaderlab";
}

bool prompt_save_project_path(HWND hwnd, const std::wstring &default_stem, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    if (!default_stem.empty()) {
        wcsncpy_s(filename, default_stem.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"ShaderLab Project (*.shaderlab;*.slab)\0*.shaderlab;*.slab\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"shaderlab";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        out_path = ensure_project_extension(filename);
        return true;
    }
    return false;
}

bool prompt_export_image_path(HWND hwnd, int default_export_format, const std::wstring &default_stem, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    if (!default_stem.empty()) {
        wcsncpy_s(filename, default_stem.c_str(), _TRUNCATE);
    }

    const wchar_t *def_ext = L"png";
    DWORD filter_index = 1;
    if (default_export_format == 1) {
        def_ext = L"jpg";
        filter_index = 2;
    } else if (default_export_format == 2) {
        def_ext = L"tga";
        filter_index = 3;
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"PNG Image (*.png)\0*.png\0"
        L"JPEG Image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"TGA Image (*.tga)\0*.tga\0"
        L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = filter_index;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        out_path = filename;
        return true;
    }
    return false;
}

bool write_rgba_image_file(
    const std::wstring &output_path,
    const uint8_t *rgba,
    uint32_t width,
    uint32_t height,
    int jpeg_quality
) {
    if (output_path.empty() || !rgba || width == 0 || height == 0) return false;

    fs::path p(output_path);
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    const std::wstring ext = to_lower_wcopy(p.extension().wstring());
    int ok = 0;

    if (ext == L".jpg" || ext == L".jpeg") {
        // Composite transparent pixels cleanly onto white/dark? Preserve RGB directly for RGBA->JPEG.
        const int q = std::clamp(jpeg_quality, 1, 100);
        ok = stbi_write_jpg_to_func(
            stb_stream_write_cb, &out,
            static_cast<int>(width), static_cast<int>(height), 4, rgba, q
        );
    } else if (ext == L".tga") {
        ok = stbi_write_tga_to_func(
            stb_stream_write_cb, &out,
            static_cast<int>(width), static_cast<int>(height), 4, rgba
        );
    } else if (ext == L".bmp") {
        ok = stbi_write_bmp_to_func(
            stb_stream_write_cb, &out,
            static_cast<int>(width), static_cast<int>(height), 4, rgba
        );
    } else {
        stbi_write_force_png_filter = 0;
        ok = stbi_write_png_to_func(
            stb_stream_write_cb, &out,
            static_cast<int>(width), static_cast<int>(height), 4, rgba,
            static_cast<int>(width * 4)
        );
    }

    out.flush();
    return (ok != 0) && out.good();
}

bool execute_save_project_archive(
    HWND hwnd,
    std::wstring &in_out_project_path,
    bool force_save_as,
    const std::wstring &source_image_path,
    const std::vector<gui::ShaderLabEffect> &effects,
    const project_file::ViewportState &view_state,
    const project_file::DepthState &depth_state,
    const std::vector<project_file::SerializedLayerEntry> &layers,
    uint32_t canvas_width,
    uint32_t canvas_height,
    int active_layer_index,
    const std::wstring &default_stem,
    std::wstring &out_error
) {
    out_error.clear();

    std::wstring target = in_out_project_path;
    if (force_save_as || target.empty() || !project_file::is_project_file(target)) {
        std::wstring chosen;
        std::wstring stem = default_stem;
        if (stem.empty() && !target.empty()) {
            stem = fs::path(target).stem().wstring();
        }
        if (!prompt_save_project_path(hwnd, stem, chosen)) {
            return false; // user cancelled
        }
        target = chosen;
    }

    target = ensure_project_extension(target);
    const std::vector<uint8_t> preset_bytes = serialize_effects_to_preset_ini(effects);

    const bool ok = project_file::save_project_v2(
        target,
        source_image_path,
        preset_bytes,
        view_state,
        depth_state,
        layers,
        canvas_width,
        canvas_height,
        active_layer_index,
        out_error
    );

    if (ok) {
        in_out_project_path = target;
    }
    return ok;
}

bool execute_export_texture(
    HWND hwnd,
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *rendered_tex,
    std::wstring &in_out_export_path,
    bool force_export_as,
    int default_export_format,
    int jpeg_quality,
    const std::wstring &default_stem,
    std::wstring &out_error
) {
    out_error.clear();

    if (!device || !context || !rendered_tex) {
        out_error = L"No rendered canvas texture available for export";
        return false;
    }

    std::wstring target = in_out_export_path;
    if (force_export_as || target.empty()) {
        std::wstring chosen;
        std::wstring stem = default_stem;
        if (stem.empty() && !target.empty()) {
            stem = fs::path(target).stem().wstring();
        }
        if (!prompt_export_image_path(hwnd, default_export_format, stem, chosen)) {
            return false; // user cancelled
        }
        target = chosen;
    }

    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!readback_texture_rgba(device, context, rendered_tex, rgba, w, h, false)) {
        out_error = L"Failed to read back rendered texture for export";
        return false;
    }

    if (!write_rgba_image_file(target, rgba.data(), w, h, jpeg_quality)) {
        out_error = L"Failed to write exported image: " + target;
        return false;
    }

    in_out_export_path = target;
    return true;
}

} // namespace lifecycle_ops
