#include "preferences_panel.h"
#include "theme.h"
#include "str_utils.h"
#include "stb_image.h"

#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace gui {

namespace {

ImVec4 to_imvec4(const float c[4], float alpha_override = -1.0f) {
    return ImVec4(c[0], c[1], c[2], (alpha_override >= 0.0f) ? alpha_override : c[3]);
}

void copy_imvec4(float dst[4], const ImVec4 &src) {
    dst[0] = src.x;
    dst[1] = src.y;
    dst[2] = src.z;
    dst[3] = src.w;
}

float color_luminance(const float c[4]) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

ImVec4 shift_color(const float c[4], float delta, float alpha = -1.0f) {
    auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    return ImVec4(
        clamp01(c[0] + delta),
        clamp01(c[1] + delta),
        clamp01(c[2] + delta),
        (alpha >= 0.0f) ? clamp01(alpha) : clamp01(c[3])
    );
}

ImVec4 mix_colors(const float a[4], const float b[4], float t, float alpha = -1.0f) {
    return ImVec4(
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        a[2] + (b[2] - a[2]) * t,
        (alpha >= 0.0f) ? alpha : (a[3] + (b[3] - a[3]) * t)
    );
}

std::string json_escape(const std::string &src) {
    std::string out;
    out.reserve(src.size() + 8);
    for (unsigned char c : src) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

std::string json_unescape(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            char next = src[++i];
            switch (next) {
            case '\"': out.push_back('\"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            default:   out.push_back(next); break;
            }
        } else {
            out.push_back(src[i]);
        }
    }
    return out;
}

size_t find_json_key(const std::string &json, const std::string &key) {
    size_t i = 0;
    const size_t n = json.size();
    while (i < n) {
        if (json[i] == '\"') {
            size_t start = i + 1;
            size_t end = start;
            while (end < n) {
                if (json[end] == '\\' && end + 1 < n) {
                    end += 2;
                    continue;
                }
                if (json[end] == '\"') break;
                ++end;
            }
            if (end >= n) break;
            std::string tok = json_unescape(json.substr(start, end - start));
            size_t colon = end + 1;
            while (colon < n && (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n')) {
                ++colon;
            }
            if (colon < n && json[colon] == ':' && tok == key) {
                return colon;
            }
            i = end + 1;
        } else {
            ++i;
        }
    }
    return std::string::npos;
}

float read_float_key(const std::string &json, const std::string &key, float def_val) {
    size_t c = find_json_key(json, key);
    if (c == std::string::npos) return def_val;
    try {
        return std::stof(json.substr(c + 1));
    } catch (...) {
        return def_val;
    }
}

int read_int_key(const std::string &json, const std::string &key, int def_val) {
    size_t c = find_json_key(json, key);
    if (c == std::string::npos) return def_val;
    try {
        return std::stoi(json.substr(c + 1));
    } catch (...) {
        return def_val;
    }
}

std::string read_str_key(const std::string &json, const std::string &key, const std::string &def_val = "") {
    size_t c = find_json_key(json, key);
    if (c == std::string::npos) return def_val;
    size_t q1 = c + 1;
    while (q1 < json.size() && (json[q1] == ' ' || json[q1] == '\t' || json[q1] == '\r' || json[q1] == '\n')) {
        ++q1;
    }
    if (q1 >= json.size() || json[q1] != '\"') return def_val;
    size_t q2 = q1 + 1;
    while (q2 < json.size()) {
        if (json[q2] == '\\' && q2 + 1 < json.size()) {
            q2 += 2;
            continue;
        }
        if (json[q2] == '\"') break;
        ++q2;
    }
    if (q2 >= json.size()) return def_val;
    return json_unescape(json.substr(q1 + 1, q2 - q1 - 1));
}

void write_rgba_json(std::ostringstream &ss, const char *prefix, const float c[4], bool trailing_comma = true) {
    ss << "  \"" << prefix << "_r\": " << c[0] << ",\n";
    ss << "  \"" << prefix << "_g\": " << c[1] << ",\n";
    ss << "  \"" << prefix << "_b\": " << c[2] << ",\n";
    ss << "  \"" << prefix << "_a\": " << c[3] << (trailing_comma ? ",\n" : "\n");
}

void read_rgba_json(const std::string &json, const char *prefix, float c[4]) {
    const std::string p(prefix);
    c[0] = std::clamp(read_float_key(json, p + "_r", c[0]), 0.0f, 1.0f);
    c[1] = std::clamp(read_float_key(json, p + "_g", c[1]), 0.0f, 1.0f);
    c[2] = std::clamp(read_float_key(json, p + "_b", c[2]), 0.0f, 1.0f);
    c[3] = std::clamp(read_float_key(json, p + "_a", c[3]), 0.0f, 1.0f);
}

void register_dom(UiDomRegistry *dom, const char *id, const char *label, const char *type, bool active = false) {
    if (!dom) return;
    dom->register_item(id, label, type, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), active, true);
}

void set_color4(float dst[4], float r, float g, float b, float a = 1.0f) {
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = a;
}

bool render_texture_slot_controls(
    HWND hwnd,
    ID3D11Device *device,
    UiDomRegistry *dom,
    const char *slot_label,
    std::wstring &tex_path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &tex_srv,
    int &tex_w,
    int &tex_h,
    int &tex_mode,
    float &tex_opacity,
    float &tex_scale,
    const char *dom_load_id,
    const char *dom_clear_id,
    const char *dom_mode_id,
    const char *dom_opacity_id,
    const char *dom_scale_id
) {
    bool modified = false;
    ImGui::PushID(slot_label);

    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", slot_label);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float clear_w = 64.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float browse_w = std::max(80.0f, avail_w - clear_w - spacing);

    if (ImGui::Button("Browse Texture...", ImVec2(browse_w, 0.0f))) {
        std::wstring chosen;
        if (open_file_dialog(
                hwnd,
                L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0",
                chosen)) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> new_srv;
            int w = 0, h = 0;
            if (load_custom_ui_texture(device, chosen, new_srv, w, h)) {
                tex_path = chosen;
                tex_srv = new_srv;
                tex_w = w;
                tex_h = h;
                modified = true;
            }
        }
    }
    register_dom(dom, dom_load_id, slot_label, "button");

    ImGui::SameLine();
    ImGui::BeginDisabled(tex_path.empty() && !tex_srv);
    if (ImGui::Button("Clear", ImVec2(clear_w, 0.0f))) {
        tex_path.clear();
        tex_srv.Reset();
        tex_w = 0;
        tex_h = 0;
        modified = true;
    }
    register_dom(dom, dom_clear_id, "Clear Texture", "button");
    ImGui::EndDisabled();

    if (!tex_path.empty()) {
        std::string name_utf8 = to_utf8(fs::path(tex_path).filename().wstring());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Active: %s (%dx%d)", name_utf8.c_str(), tex_w, tex_h);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("None loaded.");
        ImGui::PopStyleColor();
    }

    const char *modes[] = { "Tile", "Stretch", "Center" };
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("Mode", &tex_mode, modes, IM_ARRAYSIZE(modes))) {
        modified = true;
    }
    register_dom(dom, dom_mode_id, "Texture Mode", "combo");

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("Opacity", &tex_opacity, 0.0f, 1.0f, "%.2f")) {
        modified = true;
    }
    register_dom(dom, dom_opacity_id, "Texture Opacity", "slider");

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("Scale", &tex_scale, 0.25f, 4.0f, "%.2fx")) {
        modified = true;
    }
    register_dom(dom, dom_scale_id, "Texture Scale", "slider");

    ImGui::PopID();
    return modified;
}

} // namespace

std::wstring get_default_preferences_path() {
    PWSTR app_data = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app_data)) && app_data) {
        fs::path dir = fs::path(app_data) / L"ShaderLab";
        CoTaskMemFree(app_data);
        std::error_code ec;
        fs::create_directories(dir, ec);
        return (dir / L"preferences.json").wstring();
    }
    if (app_data) {
        CoTaskMemFree(app_data);
    }
    return L"preferences.json";
}

void apply_theme_preset(AppPreferences &prefs, int preset_index) {
    prefs.theme_preset = std::clamp(preset_index, 0, 5);
    switch (prefs.theme_preset) {
    case 0: {
        // Photoshop Dark Gray — initialize directly from base theme.
        apply_photoshop_gray_theme();
        const ImGuiStyle &st = ImGui::GetStyle();
        copy_imvec4(prefs.col_window_bg, st.Colors[ImGuiCol_WindowBg]);
        set_color4(prefs.col_apron_bg, 0.098f, 0.098f, 0.098f, 1.0f);
        copy_imvec4(prefs.col_frame_bg, st.Colors[ImGuiCol_FrameBg]);
        copy_imvec4(prefs.col_accent, st.Colors[ImGuiCol_CheckMark]);
        copy_imvec4(prefs.col_button, st.Colors[ImGuiCol_Button]);
        copy_imvec4(prefs.col_text, st.Colors[ImGuiCol_Text]);
        copy_imvec4(prefs.col_border, st.Colors[ImGuiCol_Border]);
        prefs.ui_rounding = 3.0f;
        prefs.ui_border_size = 1.0f;
        break;
    }
    case 1:
        // Midnight OLED Black
        set_color4(prefs.col_window_bg, 0.035f, 0.035f, 0.045f, 1.0f);
        set_color4(prefs.col_apron_bg,  0.012f, 0.012f, 0.016f, 1.0f);
        set_color4(prefs.col_frame_bg,  0.075f, 0.078f, 0.095f, 1.0f);
        set_color4(prefs.col_accent,    0.220f, 0.560f, 0.980f, 1.0f);
        set_color4(prefs.col_button,    0.110f, 0.115f, 0.140f, 1.0f);
        set_color4(prefs.col_text,      0.935f, 0.945f, 0.965f, 1.0f);
        set_color4(prefs.col_border,    0.160f, 0.170f, 0.210f, 1.0f);
        prefs.ui_rounding = 4.0f;
        prefs.ui_border_size = 1.0f;
        break;
    case 2:
        // Nordic Slate
        set_color4(prefs.col_window_bg, 0.180f, 0.204f, 0.251f, 1.0f);
        set_color4(prefs.col_apron_bg,  0.145f, 0.165f, 0.204f, 1.0f);
        set_color4(prefs.col_frame_bg,  0.231f, 0.259f, 0.322f, 1.0f);
        set_color4(prefs.col_accent,    0.533f, 0.753f, 0.816f, 1.0f);
        set_color4(prefs.col_button,    0.263f, 0.298f, 0.369f, 1.0f);
        set_color4(prefs.col_text,      0.925f, 0.937f, 0.957f, 1.0f);
        set_color4(prefs.col_border,    0.298f, 0.337f, 0.416f, 1.0f);
        prefs.ui_rounding = 4.0f;
        prefs.ui_border_size = 1.0f;
        break;
    case 3:
        // Cyberpunk Neon
        set_color4(prefs.col_window_bg, 0.071f, 0.055f, 0.125f, 1.0f);
        set_color4(prefs.col_apron_bg,  0.039f, 0.027f, 0.075f, 1.0f);
        set_color4(prefs.col_frame_bg,  0.118f, 0.086f, 0.200f, 1.0f);
        set_color4(prefs.col_accent,    0.941f, 0.188f, 0.627f, 1.0f);
        set_color4(prefs.col_button,    0.165f, 0.122f, 0.282f, 1.0f);
        set_color4(prefs.col_text,      0.910f, 0.960f, 1.000f, 1.0f);
        set_color4(prefs.col_border,    0.080f, 0.720f, 0.850f, 0.65f);
        prefs.ui_rounding = 2.0f;
        prefs.ui_border_size = 1.0f;
        break;
    case 4:
        // Light Studio
        set_color4(prefs.col_window_bg, 0.886f, 0.890f, 0.898f, 1.0f);
        set_color4(prefs.col_apron_bg,  0.780f, 0.788f, 0.804f, 1.0f);
        set_color4(prefs.col_frame_bg,  0.960f, 0.964f, 0.972f, 1.0f);
        set_color4(prefs.col_accent,    0.133f, 0.447f, 0.820f, 1.0f);
        set_color4(prefs.col_button,    0.820f, 0.831f, 0.851f, 1.0f);
        set_color4(prefs.col_text,      0.110f, 0.118f, 0.133f, 1.0f);
        set_color4(prefs.col_border,    0.675f, 0.690f, 0.718f, 1.0f);
        prefs.ui_rounding = 3.0f;
        prefs.ui_border_size = 1.0f;
        break;
    default:
        // Custom: retain current fields
        break;
    }

    apply_preferences_theme(prefs);
}

void apply_preferences_theme(const AppPreferences &prefs) {
    ImGuiStyle &style = ImGui::GetStyle();
    if (prefs.theme_preset == 0) {
        apply_photoshop_gray_theme();
    }

    const bool is_light = color_luminance(prefs.col_window_bg) > 0.5f;
    const float hover_delta = is_light ? -0.06f : 0.08f;
    const float active_delta = is_light ? -0.12f : 0.14f;
    const float chrome_delta = is_light ? -0.04f : -0.03f;

    style.Colors[ImGuiCol_WindowBg]            = to_imvec4(prefs.col_window_bg);
    style.Colors[ImGuiCol_ChildBg]             = to_imvec4(prefs.col_window_bg);
    style.Colors[ImGuiCol_PopupBg]             = shift_color(prefs.col_window_bg, is_light ? 0.04f : -0.02f, 0.98f);
    style.Colors[ImGuiCol_MenuBarBg]           = shift_color(prefs.col_window_bg, chrome_delta, 1.0f);
    style.Colors[ImGuiCol_TitleBg]             = shift_color(prefs.col_window_bg, chrome_delta, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive]       = shift_color(prefs.col_window_bg, is_light ? -0.08f : 0.02f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed]    = shift_color(prefs.col_window_bg, chrome_delta, 0.85f);

    style.Colors[ImGuiCol_Text]                = to_imvec4(prefs.col_text);
    style.Colors[ImGuiCol_TextDisabled]        = mix_colors(prefs.col_text, prefs.col_window_bg, 0.50f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg]      = to_imvec4(prefs.col_accent, 0.40f);

    style.Colors[ImGuiCol_Border]              = to_imvec4(prefs.col_border);
    style.Colors[ImGuiCol_BorderShadow]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_Separator]           = to_imvec4(prefs.col_border, 0.85f);
    style.Colors[ImGuiCol_SeparatorHovered]    = to_imvec4(prefs.col_accent, 0.85f);
    style.Colors[ImGuiCol_SeparatorActive]     = to_imvec4(prefs.col_accent, 1.0f);

    style.Colors[ImGuiCol_FrameBg]             = to_imvec4(prefs.col_frame_bg);
    style.Colors[ImGuiCol_FrameBgHovered]      = shift_color(prefs.col_frame_bg, hover_delta);
    style.Colors[ImGuiCol_FrameBgActive]       = shift_color(prefs.col_frame_bg, active_delta);

    style.Colors[ImGuiCol_Button]              = to_imvec4(prefs.col_button);
    style.Colors[ImGuiCol_ButtonHovered]       = shift_color(prefs.col_button, hover_delta);
    style.Colors[ImGuiCol_ButtonActive]        = is_light ? mix_colors(prefs.col_window_bg, prefs.col_accent, 0.35f, 1.0f) : to_imvec4(prefs.col_accent, 1.0f);

    style.Colors[ImGuiCol_Header]              = mix_colors(prefs.col_window_bg, prefs.col_accent, is_light ? 0.22f : 0.35f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered]       = mix_colors(prefs.col_window_bg, prefs.col_accent, is_light ? 0.32f : 0.65f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive]        = is_light ? mix_colors(prefs.col_window_bg, prefs.col_accent, 0.42f, 1.0f) : to_imvec4(prefs.col_accent, 1.0f);

    style.Colors[ImGuiCol_CheckMark]           = to_imvec4(prefs.col_accent, 1.0f);
    style.Colors[ImGuiCol_SliderGrab]          = to_imvec4(prefs.col_accent, 0.90f);
    style.Colors[ImGuiCol_SliderGrabActive]    = to_imvec4(prefs.col_accent, 1.0f);

    style.Colors[ImGuiCol_Tab]                 = shift_color(prefs.col_window_bg, is_light ? -0.06f : 0.04f, 1.0f);
    style.Colors[ImGuiCol_TabHovered]          = mix_colors(prefs.col_window_bg, prefs.col_accent, is_light ? 0.28f : 0.70f, 1.0f);
    style.Colors[ImGuiCol_TabActive]           = is_light ? mix_colors(prefs.col_window_bg, prefs.col_accent, 0.38f, 1.0f) : to_imvec4(prefs.col_accent, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused]        = shift_color(prefs.col_window_bg, is_light ? -0.05f : 0.02f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive]  = mix_colors(prefs.col_window_bg, prefs.col_accent, 0.55f, 1.0f);

    style.Colors[ImGuiCol_ScrollbarBg]         = shift_color(prefs.col_window_bg, is_light ? -0.05f : -0.04f, 0.9f);
    style.Colors[ImGuiCol_ScrollbarGrab]       = to_imvec4(prefs.col_button, 0.85f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]= shift_color(prefs.col_button, hover_delta);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = to_imvec4(prefs.col_accent, 1.0f);

    const float r = std::clamp(prefs.ui_rounding, 0.0f, 12.0f);
    const float b = std::clamp(prefs.ui_border_size, 0.0f, 4.0f);
    style.WindowRounding = r;
    style.ChildRounding = r;
    style.FrameRounding = r;
    style.PopupRounding = r;
    style.GrabRounding = r;
    style.TabRounding = r;
    style.ScrollbarRounding = std::max(r, 2.0f);

    style.WindowBorderSize = b;
    style.ChildBorderSize = b;
    style.PopupBorderSize = b;
    style.FrameBorderSize = b;
}

bool load_custom_ui_texture(
    ID3D11Device *device,
    const std::wstring &path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &out_srv,
    int &out_w,
    int &out_h
) {
    out_srv.Reset();
    out_w = 0;
    out_h = 0;
    if (!device || path.empty()) return false;

    std::ifstream in(fs::path(path), std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;

    std::streamsize sz = in.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return false;
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char *>(bytes.data()), sz)) return false;

    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4
    );
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = rgba;
    init_data.SysMemPitch = static_cast<UINT>(w) * 4U;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&desc, &init_data, &tex);
    stbi_image_free(rgba);
    if (FAILED(hr) || !tex) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(tex.Get(), &srv_desc, &out_srv);
    if (FAILED(hr) || !out_srv) {
        out_srv.Reset();
        return false;
    }

    out_w = w;
    out_h = h;
    return true;
}

void draw_custom_panel_texture(
    ImDrawList *draw_list,
    const ImVec2 &p_min,
    const ImVec2 &p_max,
    ID3D11ShaderResourceView *srv,
    int tex_w,
    int tex_h,
    int mode,
    float opacity,
    float scale
) {
    if (!draw_list || !srv || tex_w <= 0 || tex_h <= 0) return;
    if (p_max.x <= p_min.x || p_max.y <= p_min.y) return;

    const float clamped_alpha = std::clamp(opacity, 0.0f, 1.0f);
    if (clamped_alpha <= 0.001f) return;

    const float clamped_scale = std::clamp(scale, 0.1f, 8.0f);
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(clamped_alpha * 255.0f));
    const ImTextureID tex_id = reinterpret_cast<ImTextureID>(srv);

    draw_list->PushClipRect(p_min, p_max, true);

    if (mode == 1) {
        // Stretch to bounds
        draw_list->AddImage(tex_id, p_min, p_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
    } else if (mode == 2) {
        // Center with scale
        const float draw_w = static_cast<float>(tex_w) * clamped_scale;
        const float draw_h = static_cast<float>(tex_h) * clamped_scale;
        const float cx = (p_min.x + p_max.x) * 0.5f;
        const float cy = (p_min.y + p_max.y) * 0.5f;
        const ImVec2 q0(cx - draw_w * 0.5f, cy - draw_h * 0.5f);
        const ImVec2 q1(cx + draw_w * 0.5f, cy + draw_h * 0.5f);
        draw_list->AddImage(tex_id, q0, q1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
    } else {
        // Tile using explicit tile quads so DX11 CLAMP samplers still tile cleanly.
        const float tile_w = std::max(16.0f, static_cast<float>(tex_w) * clamped_scale);
        const float tile_h = std::max(16.0f, static_cast<float>(tex_h) * clamped_scale);
        const float region_w = p_max.x - p_min.x;
        const float region_h = p_max.y - p_min.y;
        const int cols = std::min(48, static_cast<int>(std::ceil(region_w / tile_w)));
        const int rows = std::min(48, static_cast<int>(std::ceil(region_h / tile_h)));
        for (int r = 0; r < rows; ++r) {
            const float y0 = p_min.y + static_cast<float>(r) * tile_h;
            const float y1 = y0 + tile_h;
            for (int c = 0; c < cols; ++c) {
                const float x0 = p_min.x + static_cast<float>(c) * tile_w;
                const float x1 = x0 + tile_w;
                draw_list->AddImage(tex_id, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
            }
        }
    }

    draw_list->PopClipRect();
}

bool save_preferences_json(const AppPreferences &prefs, const std::wstring &path) {
    const std::wstring target = path.empty() ? get_default_preferences_path() : path;
    fs::path p(target);
    std::error_code ec;
    if (!p.parent_path().empty()) {
        fs::create_directories(p.parent_path(), ec);
    }

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"theme_preset\": " << prefs.theme_preset << ",\n";
    write_rgba_json(ss, "col_window_bg", prefs.col_window_bg);
    write_rgba_json(ss, "col_apron_bg", prefs.col_apron_bg);
    write_rgba_json(ss, "col_frame_bg", prefs.col_frame_bg);
    write_rgba_json(ss, "col_accent", prefs.col_accent);
    write_rgba_json(ss, "col_button", prefs.col_button);
    write_rgba_json(ss, "col_text", prefs.col_text);
    write_rgba_json(ss, "col_border", prefs.col_border);
    ss << "  \"ui_rounding\": " << prefs.ui_rounding << ",\n";
    ss << "  \"ui_border_size\": " << prefs.ui_border_size << ",\n";
    ss << "  \"panel_texture_path\": \"" << json_escape(str_utils::wide_to_utf8(prefs.panel_texture_path)) << "\",\n";
    ss << "  \"panel_texture_mode\": " << prefs.panel_texture_mode << ",\n";
    ss << "  \"panel_texture_opacity\": " << prefs.panel_texture_opacity << ",\n";
    ss << "  \"panel_texture_scale\": " << prefs.panel_texture_scale << ",\n";
    ss << "  \"apron_texture_path\": \"" << json_escape(str_utils::wide_to_utf8(prefs.apron_texture_path)) << "\",\n";
    ss << "  \"apron_texture_mode\": " << prefs.apron_texture_mode << ",\n";
    ss << "  \"apron_texture_opacity\": " << prefs.apron_texture_opacity << ",\n";
    ss << "  \"apron_texture_scale\": " << prefs.apron_texture_scale << ",\n";
    ss << "  \"checker_size\": " << prefs.checker_size << ",\n";
    write_rgba_json(ss, "checker_col_a", prefs.checker_col_a);
    write_rgba_json(ss, "checker_col_b", prefs.checker_col_b);
    ss << "  \"default_export_format\": " << prefs.default_export_format << ",\n";
    ss << "  \"export_jpeg_quality\": " << prefs.export_jpeg_quality << ",\n";
    ss << "  \"autosave_interval_min\": " << prefs.autosave_interval_min << "\n";
    ss << "}\n";

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    const std::string data = ss.str();
    out.write(data.data(), data.size());
    out.flush();
    return out.good();
}

bool load_preferences_json(ID3D11Device *device, AppPreferences &prefs, const std::wstring &path) {
    const std::wstring target = path.empty() ? get_default_preferences_path() : path;
    std::ifstream in(fs::path(target), std::ios::binary);
    if (!in.is_open()) {
        apply_theme_preset(prefs, 0);
        return false;
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string json = buf.str();
    if (json.empty()) {
        apply_theme_preset(prefs, 0);
        return false;
    }

    int preset = std::clamp(read_int_key(json, "theme_preset", 0), 0, 5);
    apply_theme_preset(prefs, preset);

    read_rgba_json(json, "col_window_bg", prefs.col_window_bg);
    read_rgba_json(json, "col_apron_bg", prefs.col_apron_bg);
    read_rgba_json(json, "col_frame_bg", prefs.col_frame_bg);
    read_rgba_json(json, "col_accent", prefs.col_accent);
    read_rgba_json(json, "col_button", prefs.col_button);
    read_rgba_json(json, "col_text", prefs.col_text);
    read_rgba_json(json, "col_border", prefs.col_border);

    prefs.ui_rounding = std::clamp(read_float_key(json, "ui_rounding", prefs.ui_rounding), 0.0f, 12.0f);
    prefs.ui_border_size = std::clamp(read_float_key(json, "ui_border_size", prefs.ui_border_size), 0.0f, 4.0f);

    prefs.panel_texture_path = str_utils::utf8_to_wide(read_str_key(json, "panel_texture_path"));
    prefs.panel_texture_mode = std::clamp(read_int_key(json, "panel_texture_mode", 0), 0, 2);
    prefs.panel_texture_opacity = std::clamp(read_float_key(json, "panel_texture_opacity", 0.25f), 0.0f, 1.0f);
    prefs.panel_texture_scale = std::clamp(read_float_key(json, "panel_texture_scale", 1.0f), 0.25f, 4.0f);

    prefs.apron_texture_path = str_utils::utf8_to_wide(read_str_key(json, "apron_texture_path"));
    prefs.apron_texture_mode = std::clamp(read_int_key(json, "apron_texture_mode", 0), 0, 2);
    prefs.apron_texture_opacity = std::clamp(read_float_key(json, "apron_texture_opacity", 0.25f), 0.0f, 1.0f);
    prefs.apron_texture_scale = std::clamp(read_float_key(json, "apron_texture_scale", 1.0f), 0.25f, 4.0f);

    prefs.checker_size = std::clamp(read_float_key(json, "checker_size", 16.0f), 4.0f, 64.0f);
    read_rgba_json(json, "checker_col_a", prefs.checker_col_a);
    read_rgba_json(json, "checker_col_b", prefs.checker_col_b);

    prefs.default_export_format = std::clamp(read_int_key(json, "default_export_format", 0), 0, 2);
    prefs.export_jpeg_quality = std::clamp(read_int_key(json, "export_jpeg_quality", 92), 10, 100);
    prefs.autosave_interval_min = read_int_key(json, "autosave_interval_min", 0);

    if (device) {
        if (!prefs.panel_texture_path.empty()) {
            load_custom_ui_texture(
                device, prefs.panel_texture_path,
                prefs.panel_texture_srv, prefs.panel_texture_w, prefs.panel_texture_h
            );
        }
        if (!prefs.apron_texture_path.empty()) {
            load_custom_ui_texture(
                device, prefs.apron_texture_path,
                prefs.apron_texture_srv, prefs.apron_texture_w, prefs.apron_texture_h
            );
        }
    }

    apply_preferences_theme(prefs);
    return true;
}

bool render_preferences_tab(
    HWND hwnd,
    ID3D11Device *device,
    AppPreferences &prefs,
    UiDomRegistry *dom,
    const std::function<void()> &on_changed
) {
    bool modified = false;
    bool theme_modified = false;

    const ImGuiStyle &st = ImGui::GetStyle();
    const float footer_h = ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y + 4.0f;

    if (ImGui::BeginChild("PrefsScrollBody", ImVec2(0.0f, -footer_h), false)) {
        // Section 1: Theme Presets & Colors
        if (ImGui::CollapsingHeader("Theme & Colors", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Preset");
            const char *preset_names[] = {
                "Photoshop Dark Gray",
                "Midnight OLED Black",
                "Nordic Slate",
                "Cyberpunk Neon",
                "Light Studio",
                "Custom"
            };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##prefs_preset", &prefs.theme_preset, preset_names, IM_ARRAYSIZE(preset_names))) {
                if (prefs.theme_preset != 5) {
                    apply_theme_preset(prefs, prefs.theme_preset);
                } else {
                    apply_preferences_theme(prefs);
                }
                modified = true;
                theme_modified = true;
            }
            register_dom(dom, "prefs_combo_preset", "Theme Preset", "combo");

            const ImGuiColorEditFlags col_flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;
            struct ColorBinding {
                const char *label;
                float *rgba;
                const char *dom_id;
            };
            ColorBinding color_rows[] = {
                { "Window / Panel BG",  prefs.col_window_bg, "prefs_color_window_bg" },
                { "Canvas Apron BG",    prefs.col_apron_bg,  "prefs_color_apron" },
                { "Frame / Input BG",   prefs.col_frame_bg,  "prefs_color_frame_bg" },
                { "Accent / Active",    prefs.col_accent,    "prefs_color_accent" },
                { "Buttons",            prefs.col_button,    "prefs_color_button" },
                { "Text",               prefs.col_text,      "prefs_color_text" },
                { "Borders",            prefs.col_border,    "prefs_color_border" }
            };

            for (const auto &row : color_rows) {
                ImGui::AlignTextToFramePadding();
                if (ImGui::ColorEdit4(row.label, row.rgba, col_flags)) {
                    prefs.theme_preset = 5;
                    theme_modified = true;
                    modified = true;
                }
                register_dom(dom, row.dom_id, row.label, "color_picker");
            }

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("Corner Rounding", &prefs.ui_rounding, 0.0f, 10.0f, "%.1f px")) {
                theme_modified = true;
                modified = true;
            }
            register_dom(dom, "prefs_slider_rounding", "Corner Rounding", "slider");

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("Border Size", &prefs.ui_border_size, 0.0f, 3.0f, "%.1f px")) {
                theme_modified = true;
                modified = true;
            }
            register_dom(dom, "prefs_slider_border", "Border Size", "slider");
        }

        // Section 2: Custom Panel & Canvas Apron Textures
        if (ImGui::CollapsingHeader("Custom UI Textures")) {
            ImGui::TextWrapped("Overlay subtle grain, brushed metal, or custom artwork behind UI panels and the canvas apron.");
            ImGui::Spacing();

            if (render_texture_slot_controls(
                    hwnd, device, dom, "Side Panel Texture",
                    prefs.panel_texture_path, prefs.panel_texture_srv,
                    prefs.panel_texture_w, prefs.panel_texture_h,
                    prefs.panel_texture_mode, prefs.panel_texture_opacity, prefs.panel_texture_scale,
                    "prefs_btn_load_panel_tex", "prefs_btn_clear_panel_tex",
                    "prefs_combo_panel_tex_mode", "prefs_slider_panel_tex_opacity", "prefs_slider_panel_tex_scale")) {
                modified = true;
            }

            ImGui::Separator();

            if (render_texture_slot_controls(
                    hwnd, device, dom, "Canvas Apron Texture",
                    prefs.apron_texture_path, prefs.apron_texture_srv,
                    prefs.apron_texture_w, prefs.apron_texture_h,
                    prefs.apron_texture_mode, prefs.apron_texture_opacity, prefs.apron_texture_scale,
                    "prefs_btn_load_apron_tex", "prefs_btn_clear_apron_tex",
                    "prefs_combo_apron_tex_mode", "prefs_slider_apron_tex_opacity", "prefs_slider_apron_tex_scale")) {
                modified = true;
            }
        }

        // Section 3: Transparency Checkerboard
        if (ImGui::CollapsingHeader("Transparency Checkerboard", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("Square Size", &prefs.checker_size, 4.0f, 64.0f, "%.0f px")) {
                modified = true;
            }
            register_dom(dom, "prefs_slider_checker_size", "Checkerboard Size", "slider");

            const ImGuiColorEditFlags chk_flags = ImGuiColorEditFlags_NoInputs;
            ImGui::AlignTextToFramePadding();
            if (ImGui::ColorEdit4("Checker Color A", prefs.checker_col_a, chk_flags)) {
                modified = true;
            }
            register_dom(dom, "prefs_color_checker_a", "Checker Color A", "color_picker");

            ImGui::AlignTextToFramePadding();
            if (ImGui::ColorEdit4("Checker Color B", prefs.checker_col_b, chk_flags)) {
                modified = true;
            }
            register_dom(dom, "prefs_color_checker_b", "Checker Color B", "color_picker");
        }

        // Section 4: Export & Autosave Defaults
        if (ImGui::CollapsingHeader("Export & Autosave", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Default Export Format");
            const char *export_formats[] = { "PNG (.png)", "JPEG (.jpg)", "TGA (.tga)" };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##prefs_export_fmt", &prefs.default_export_format, export_formats, IM_ARRAYSIZE(export_formats))) {
                modified = true;
            }
            register_dom(dom, "prefs_combo_export_fmt", "Default Export Format", "combo");

            if (prefs.default_export_format == 1) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("JPEG Quality", &prefs.export_jpeg_quality, 10, 100, "%d%%")) {
                    modified = true;
                }
                register_dom(dom, "prefs_slider_jpg_quality", "JPEG Quality", "slider");
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Autosave Interval");
            const int intervals[] = { 0, 1, 5, 15 };
            const char *interval_labels[] = { "Off", "Every 1 minute", "Every 5 minutes", "Every 15 minutes" };
            int interval_idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (prefs.autosave_interval_min == intervals[i]) {
                    interval_idx = i;
                    break;
                }
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##prefs_autosave", &interval_idx, interval_labels, IM_ARRAYSIZE(interval_labels))) {
                prefs.autosave_interval_min = intervals[interval_idx];
                modified = true;
            }
            register_dom(dom, "prefs_combo_autosave", "Autosave Interval", "combo");
        }
    }
    ImGui::EndChild();

    // Pinned footer bar: Save + Reset Theme
    ImGui::Separator();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float half_w = std::max(60.0f, (avail_w - st.ItemSpacing.x) * 0.5f);

    if (ImGui::Button("Save Preferences", ImVec2(half_w, 0.0f))) {
        save_preferences_json(prefs);
        modified = true;
    }
    register_dom(dom, "prefs_btn_save", "Save Preferences", "button");

    ImGui::SameLine();
    if (ImGui::Button("Reset Theme", ImVec2(half_w, 0.0f))) {
        apply_theme_preset(prefs, 0);
        prefs.panel_texture_path.clear();
        prefs.panel_texture_srv.Reset();
        prefs.panel_texture_w = 0;
        prefs.panel_texture_h = 0;
        prefs.apron_texture_path.clear();
        prefs.apron_texture_srv.Reset();
        prefs.apron_texture_w = 0;
        prefs.apron_texture_h = 0;
        prefs.checker_size = 16.0f;
        set_color4(prefs.checker_col_a, 0.80f, 0.80f, 0.80f, 1.0f);
        set_color4(prefs.checker_col_b, 0.55f, 0.55f, 0.55f, 1.0f);
        save_preferences_json(prefs);
        theme_modified = true;
        modified = true;
    }
    register_dom(dom, "prefs_btn_reset_theme", "Reset Theme", "button");

    if (theme_modified) {
        apply_preferences_theme(prefs);
    }
    if (modified && on_changed) {
        on_changed();
    }
    return modified;
}

} // namespace gui
