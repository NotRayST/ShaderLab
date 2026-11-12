#pragma once
#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>

namespace gui {

enum class ActiveTool {
    Move,           // V - Intra-frame layer translation (Feature 8)
    Hand,           // H - Viewport camera pan/zoom
    Rotate,         // R - Rotate + Camera Raw Horizon Alignment line (Feature 6)
    Crop,           // C - Crop marquee with aspect ratio presets (Feature 13)
    RasterErase,    // E - Alpha Eraser with Size, Hardness, Opacity (Feature 12)
    AiInpaint,      // J - LaMa Big HD Object Erase (renamed from Erase, Feature 7 & 12)
    Erase = AiInpaint, // temporary alias so existing code builds before @canvas-tools updates refs
    Text,           // T - System Font Text Layer Tool (Feature 15)
    Upscale,        // U - AI Super-Resolution Upscaler (Feature 14)
    Depth,          // D - 3D Depth Calibration
    Split           // B - Before/After Split Comparison
};

enum class ToolIconType {
    Move,
    Hand,
    Rotate,
    Crop,
    RasterErase,
    AiInpaint,
    Erase = AiInpaint, // temporary alias for phase 0 compilation
    Text,
    Upscale,
    Depth,
    Split,
    Bypass
};

enum class BlendMode : int {
    Normal = 0,
    Dissolve = 1,
    Darken = 2,
    Multiply = 3,
    ColorBurn = 4,
    LinearBurn = 5,
    DarkerColor = 6,
    Lighten = 7,
    Screen = 8,
    ColorDodge = 9,
    LinearDodge = 10,
    LighterColor = 11,
    Overlay = 12,
    SoftLight = 13,
    HardLight = 14,
    VividLight = 15,
    LinearLight = 16,
    PinLight = 17,
    HardMix = 18,
    Difference = 19,
    Exclusion = 20,
    Subtract = 21,
    Divide = 22,
    Count = 23
};

struct ShaderLabUniform {
    enum class Type { Float, Int, Bool, Color3, Color4, Enum };
    std::string name;
    std::string label;
    std::string tooltip;
    std::string category;
    Type type = Type::Float;
    float min_val = 0.0f;
    float max_val = 1.0f;
    float step = 0.01f;
    float float_val = 0.5f;
    float default_float = 0.5f;
    int int_val = 0;
    int default_int = 0;
    bool bool_val = false;
    bool default_bool = false;
    float color_val[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float default_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<std::string> enum_items;
    int enum_selected = 0;
    int default_enum = 0;

    void reset_to_default() {
        float_val = default_float;
        int_val = default_int;
        bool_val = default_bool;
        for (int i = 0; i < 4; ++i) color_val[i] = default_color[i];
        enum_selected = default_enum;
    }
};

struct ShaderLabEffect {
    std::string name;
    std::string file_path;
    std::string engine_type;
    bool enabled = true;
    std::vector<ShaderLabUniform> uniforms;

    void reset_uniforms() {
        for (auto &u : uniforms) {
            u.reset_to_default();
        }
    }
};

struct DocumentTab {
    uint32_t id = 0;
    std::wstring file_path;
    std::wstring display_name;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_depth = false;
    bool is_dirty = false;
};

struct UiDomNode {
    std::string id;
    std::string label;
    std::string type;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    bool active = false;
    bool enabled = false;
};

class UiDomRegistry {
public:
    void clear() { m_nodes.clear(); }
    void register_item(const std::string &id, const std::string &label, const std::string &type,
                       const ImVec2 &p_min, const ImVec2 &p_max, bool active = false, bool enabled = true) {
        UiDomNode node;
        node.id = id;
        node.label = label;
        node.type = type;
        node.x = p_min.x;
        node.y = p_min.y;
        node.w = p_max.x - p_min.x;
        node.h = p_max.y - p_min.y;
        node.active = active;
        node.enabled = enabled;
        m_nodes.push_back(node);
    }

    const std::vector<UiDomNode> &get_nodes() const { return m_nodes; }

    const UiDomNode *find_direct(const std::string &query) const {
        for (const auto &n : m_nodes) {
            if (n.id == query) return &n;
        }
        for (const auto &n : m_nodes) {
            if (_stricmp(n.id.c_str(), query.c_str()) == 0) return &n;
        }
        return nullptr;
    }

    const UiDomNode *find(const std::string &query) const {
        if (const UiDomNode *direct = find_direct(query)) return direct;

        std::string q_low = query;
        std::transform(q_low.begin(), q_low.end(), q_low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        struct AliasEntry {
            const char *alias;
            const char *target;
        };
        static constexpr AliasEntry kAliases[] = {
            { "tool_move",       "tool_V" },
            { "tool_v",          "tool_move" },
            { "tool_hand",       "tool_H" },
            { "tool_h",          "tool_hand" },
            { "tool_rotate",     "tool_R" },
            { "tool_r",          "tool_rotate" },
            { "tool_crop",       "tool_C" },
            { "tool_c",          "tool_crop" },
            { "tool_eraser",     "tool_E" },
            { "tool_eraser",     "tool_erase" },
            { "tool_erase",      "tool_E" },
            { "tool_erase",      "tool_eraser" },
            { "tool_e",          "tool_eraser" },
            { "tool_e",          "tool_erase" },
            { "tool_ai_inpaint", "tool_J" },
            { "tool_j",          "tool_ai_inpaint" },
            { "tool_text",       "tool_T" },
            { "tool_t",          "tool_text" },
            { "tool_upscale",    "tool_U" },
            { "tool_u",          "tool_upscale" },
            { "tool_depth",      "tool_D" },
            { "tool_d",          "tool_depth" },
            { "tool_split",      "tool_B" },
            { "tool_b",          "tool_split" },
            { "tool_bypass",     "tool_\\" },
            { "tool_\\",         "tool_bypass" },
            { "accordion_depth", "tab_depth" },
            { "tab_depth",       "accordion_depth" },
            { "accordion_erase", "tab_ai_inpaint" },
            { "accordion_erase", "tab_erase" },
            { "tab_erase",       "tab_ai_inpaint" },
            { "tab_erase",       "accordion_erase" },
            { "tab_ai_inpaint",  "tab_erase" },
            { "tab_ai_inpaint",  "accordion_erase" },
            { "tab_prefs",       "tab_preferences" },
            { "tab_preferences", "tab_prefs" },
            { "tab_upscale",     "accordion_upscale" },
            { "tab_text",        "accordion_text" },
            { "opt_fit",         "status_btn_fit" },
            { "opt_100",         "status_btn_100" },
            { "opt_200",         "status_btn_200" },
        };
        for (const auto &entry : kAliases) {
            if (q_low == entry.alias) {
                if (const UiDomNode *n = find_direct(entry.target)) return n;
            }
        }
        for (const auto &n : m_nodes) {
            if (_stricmp(n.label.c_str(), query.c_str()) == 0) return &n;
        }
        for (const auto &n : m_nodes) {
            if (n.label.find(query) != std::string::npos) return &n;
        }
        return nullptr;
    }

    std::string to_json() const {
        std::string out = "{\n  \"elements\": [\n";
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            const auto &n = m_nodes[i];
            out += "    {\"id\": \"" + json_escape(n.id) + "\", ";
            out += "\"label\": \"" + json_escape(n.label) + "\", ";
            out += "\"type\": \"" + json_escape(n.type) + "\", ";
            out += "\"rect\": [" + std::to_string((int)n.x) + ", " + std::to_string((int)n.y) + ", " +
                   std::to_string((int)n.w) + ", " + std::to_string((int)n.h) + "], ";
            out += "\"active\": " + std::string(n.active ? "true" : "false") + ", ";
            out += "\"enabled\": " + std::string(n.enabled ? "true" : "false") + "}";
            if (i + 1 < m_nodes.size()) out += ",";
            out += "\n";
        }
        out += "  ]\n}\n";
        return out;
    }

private:
    static std::string json_escape(const std::string &s) {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    }

    std::vector<UiDomNode> m_nodes;
};

inline std::string to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

inline bool open_file_dialog(HWND hwnd, const wchar_t *filter, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        out_path = filename;
        return true;
    }
    return false;
}

inline bool save_file_dialog(HWND hwnd, const wchar_t *filter, const wchar_t *def_ext, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
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

inline void BulletTextWrapped(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
}

} // namespace gui
