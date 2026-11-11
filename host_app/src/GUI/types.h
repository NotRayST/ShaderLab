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
#include <cmath>
#include <cstdarg>

namespace gui {

enum class ActiveTool {
    Hand,
    Erase,
    Depth,
    Split
};

enum class ToolIconType {
    Hand,
    Erase,
    Depth,
    Split,
    Bypass
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

    const UiDomNode *find(const std::string &query) const {
        for (const auto &n : m_nodes) {
            if (n.id == query) return &n;
        }
        for (const auto &n : m_nodes) {
            if (_stricmp(n.id.c_str(), query.c_str()) == 0) return &n;
        }
        // Aliases for tools and tabs
        if (query == "tool_hand" || query == "tool_h") return find("tool_H");
        if (query == "tool_erase" || query == "tool_e") return find("tool_E");
        if (query == "tool_depth" || query == "tool_d") return find("tool_D");
        if (query == "tool_split" || query == "tool_b") return find("tool_B");
        if (query == "tool_bypass" || query == "tool_\\") return find("tool_\\");
        if (query == "accordion_depth") {
            const UiDomNode *n = find("tab_depth");
            if (n) return n;
        }
        if (query == "accordion_erase") {
            const UiDomNode *n = find("tab_erase");
            if (n) return n;
        }
        if (query == "opt_fit") { const UiDomNode *n = find("status_btn_fit"); if (n) return n; }
        if (query == "opt_100") { const UiDomNode *n = find("status_btn_100"); if (n) return n; }
        if (query == "opt_200") { const UiDomNode *n = find("status_btn_200"); if (n) return n; }
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
