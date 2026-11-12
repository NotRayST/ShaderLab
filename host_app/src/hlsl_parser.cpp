#include "hlsl_parser.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace shaderlab {
namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim_copy(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool is_ident_start(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

bool is_ident_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return is_ident_start(c) || (u >= '0' && u <= '9');
}

bool starts_with_word(const std::string &s, size_t pos, const char *word) {
    size_t wlen = std::strlen(word);
    if (pos + wlen > s.size()) return false;
    if (pos > 0 && is_ident_char(s[pos - 1])) return false;
    if (s.compare(pos, wlen, word) != 0) return false;
    if (pos + wlen < s.size() && is_ident_char(s[pos + wlen])) return false;
    return true;
}

size_t skip_ws_and_comments(const std::string &s, size_t i) {
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = (i + 1 < s.size()) ? i + 2 : s.size();
        } else {
            break;
        }
    }
    return i;
}

std::string strip_comments_preserve_strings(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '"') {
            out.push_back(src[i++]);
            while (i < src.size()) {
                char c = src[i++];
                out.push_back(c);
                if (c == '\\' && i < src.size()) {
                    out.push_back(src[i++]);
                } else if (c == '"') {
                    break;
                }
            }
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') out.push_back('\n');
                ++i;
            }
            i = (i + 1 < src.size()) ? i + 2 : src.size();
            out.push_back(' ');
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

size_t find_matching_delim(const std::string &s, size_t open_pos, char open_c, char close_c) {
    if (open_pos >= s.size() || s[open_pos] != open_c) return std::string::npos;
    int depth = 0;
    size_t i = open_pos;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else if (s[i++] == '"') break;
            }
            continue;
        }
        if (i + 1 < s.size() && c == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < s.size() && c == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (c == open_c) {
            ++depth;
        } else if (c == close_c) {
            --depth;
            if (depth == 0) return i;
        }
        ++i;
    }
    return std::string::npos;
}

// ReShade annotations use < ... > which may contain >= or <= inside expressions,
// so parse string literals and track parentheses when closing '>'.
size_t find_matching_angle(const std::string &s, size_t open_pos) {
    if (open_pos >= s.size() || s[open_pos] != '<') return std::string::npos;
    int paren = 0;
    int brace = 0;
    size_t i = open_pos + 1;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else if (s[i++] == '"') break;
            }
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '>' && paren == 0 && brace == 0) {
            return i;
        }
        ++i;
    }
    return std::string::npos;
}

std::vector<std::string> split_top_level_args(const std::string &s) {
    std::vector<std::string> out;
    int paren = 0;
    int brace = 0;
    int bracket = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else if (s[i] == '"') break;
                else ++i;
            }
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        else if (c == ',' && paren == 0 && brace == 0 && bracket == 0) {
            out.push_back(trim_copy(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    std::string tail = trim_copy(s.substr(start));
    if (!tail.empty() || !out.empty()) out.push_back(tail);
    return out;
}

std::string decode_escaped_string_sequence(const std::string &raw, bool keep_embedded_nul) {
    // Handles adjacent C string literals: "foo\0" "bar\0"
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && raw[i] != '"') ++i;
        if (i >= raw.size()) break;
        ++i;
        while (i < raw.size() && raw[i] != '"') {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char esc = raw[i + 1];
                i += 2;
                if (esc == 'n') out.push_back('\n');
                else if (esc == 'r') out.push_back('\r');
                else if (esc == 't') out.push_back('\t');
                else if (esc == '\\') out.push_back('\\');
                else if (esc == '"') out.push_back('"');
                else if (esc == '0') {
                    out.push_back(keep_embedded_nul ? '\0' : '\n');
                } else {
                    out.push_back(esc);
                }
            } else {
                out.push_back(raw[i++]);
            }
        }
        if (i < raw.size() && raw[i] == '"') ++i;
    }
    return out;
}

// Embedded virtual ReShade headers used only if not present on disk
const char *kFallbackReShadeUiFxh = R"FXH(
#pragma once
#define __UNIFORM_SLIDER_FLOAT1 ui_type = "slider";
#define __UNIFORM_SLIDER_INT1 ui_type = "slider";
#define __UNIFORM_DRAG_FLOAT1 ui_type = "drag";
#define __UNIFORM_DRAG_INT1 ui_type = "drag";
#define __UNIFORM_INPUT_FLOAT1 ui_type = "input";
#define __UNIFORM_INPUT_INT1 ui_type = "input";
#define __UNIFORM_COMBO_INT1 ui_type = "combo";
#define __UNIFORM_COMBO_BOOL1 ui_type = "combo";
#define __UNIFORM_COLOR_FLOAT3 ui_type = "color";
#define __UNIFORM_COLOR_FLOAT4 ui_type = "color";
#define __UNIFORM_RADIO_INT1 ui_type = "radio";
)FXH";

const char *kFallbackReShadeFxh = R"FXH(
#pragma once
#ifndef BUFFER_RCP_WIDTH
#define BUFFER_RCP_WIDTH (1.0 / BUFFER_WIDTH)
#endif
#ifndef BUFFER_RCP_HEIGHT
#define BUFFER_RCP_HEIGHT (1.0 / BUFFER_HEIGHT)
#endif
#ifndef BUFFER_PIXEL_SIZE
#define BUFFER_PIXEL_SIZE float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)
#endif
#ifndef BUFFER_SCREEN_SIZE
#define BUFFER_SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)
#endif
#ifndef BUFFER_ASPECT_RATIO
#define BUFFER_ASPECT_RATIO (BUFFER_WIDTH * BUFFER_RCP_HEIGHT)
#endif

namespace ReShade
{
    static const float AspectRatio = BUFFER_ASPECT_RATIO;
    static const float2 PixelSize = BUFFER_PIXEL_SIZE;
    static const float2 ScreenSize = BUFFER_SCREEN_SIZE;

    texture BackBufferTex : COLOR;
    texture DepthBufferTex : DEPTH;

    sampler BackBuffer { Texture = BackBufferTex; };
    sampler DepthBuffer { Texture = DepthBufferTex; };

    float GetLinearizedDepth(float2 texcoord)
    {
        float depth = tex2Dlod(DepthBuffer, float4(texcoord, 0, 0)).x;
        return saturate(depth);
    }
}

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)FXH";

// -------------------- Preprocessor Engine --------------------

struct MacroDef {
    bool is_func = false;
    bool variadic = false;
    std::vector<std::string> params;
    std::string body;
};

class Preprocessor {
public:
    Preprocessor(
        const std::filesystem::path &base_file,
        uint32_t buf_w,
        uint32_t buf_h,
        std::vector<std::string> &warnings)
        : m_base_dir(base_file.parent_path()), m_buf_w(buf_w ? buf_w : 1920), m_buf_h(buf_h ? buf_h : 1080), m_warnings(warnings)
    {
        define_simple("__RESHADE__", "60100");
        define_simple("__RESHADE_PERFORMANCE_MODE__", "0");
        define_simple("__RENDERER__", "0xb000");
        define_simple("__VENDOR__", "0x10de");
        define_simple("__DEVICE__", "0");
        define_simple("BUFFER_COLOR_SPACE", "1");
        define_simple("BUFFER_COLOR_BIT_DEPTH", "8");
        build_include_search_dirs(base_file);
    }

    void define_simple(const std::string &name, const std::string &val) {
        MacroDef m;
        m.is_func = false;
        m.body = val;
        m_macros[name] = m;
    }

    std::string run(const std::string &source, const std::filesystem::path &current_file, int depth = 0) {
        if (depth > 32) {
            m_warnings.push_back("Include recursion depth exceeded for " + current_file.filename().string());
            return {};
        }

        // Join backslash continuations before splitting lines
        std::string normalized;
        normalized.reserve(source.size());
        for (size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\r') continue;
            if (source[i] == '\\' && i + 1 < source.size() && source[i + 1] == '\n') {
                ++i;
                continue;
            }
            if (source[i] == '\\' && i + 2 < source.size() && source[i + 1] == '\r' && source[i + 2] == '\n') {
                i += 2;
                continue;
            }
            normalized.push_back(source[i]);
        }

        std::string cleaned = strip_comments_preserve_strings(normalized);
        std::stringstream ss(cleaned);
        std::string line;
        std::ostringstream out;

        struct CondFrame {
            bool parent_active = true;
            bool this_active = true;
            bool any_taken = true;
            bool seen_else = false;
        };
        std::vector<CondFrame> stack;
        auto is_active = [&]() {
            return stack.empty() ? true : (stack.back().parent_active && stack.back().this_active);
        };

        // Accumulate active non-directive spans so multi-line function-like macro calls expand intact
        std::ostringstream active_chunk;
        auto flush_chunk = [&]() {
            std::string c = active_chunk.str();
            if (!c.empty()) {
                std::unordered_set<std::string> disabled;
                out << expand_macros(c, disabled);
                active_chunk.str("");
                active_chunk.clear();
            }
        };

        while (std::getline(ss, line)) {
            std::string trimmed = trim_copy(line);
            if (!trimmed.empty() && trimmed[0] == '#') {
                flush_chunk();
                std::string rest = trim_copy(trimmed.substr(1));
                size_t sp = 0;
                while (sp < rest.size() && is_ident_char(rest[sp])) ++sp;
                std::string dir = rest.substr(0, sp);
                std::string arg = trim_copy(rest.substr(sp));

                if (dir == "if" || dir == "ifdef" || dir == "ifndef") {
                    bool parent = is_active();
                    bool cond = false;
                    if (parent) {
                        if (dir == "ifdef") cond = is_defined_macro(trim_copy(arg));
                        else if (dir == "ifndef") cond = !is_defined_macro(trim_copy(arg));
                        else cond = eval_pp_condition(arg);
                    }
                    CondFrame f;
                    f.parent_active = parent;
                    f.this_active = cond;
                    f.any_taken = cond;
                    stack.push_back(f);
                    continue;
                }
                if (dir == "elif") {
                    if (!stack.empty() && !stack.back().seen_else) {
                        auto &top = stack.back();
                        if (!top.parent_active || top.any_taken) {
                            top.this_active = false;
                        } else {
                            bool cond = eval_pp_condition(arg);
                            top.this_active = cond;
                            top.any_taken = cond;
                        }
                    }
                    continue;
                }
                if (dir == "else") {
                    if (!stack.empty() && !stack.back().seen_else) {
                        auto &top = stack.back();
                        top.seen_else = true;
                        top.this_active = top.parent_active && !top.any_taken;
                        top.any_taken = true;
                    }
                    continue;
                }
                if (dir == "endif") {
                    if (!stack.empty()) stack.pop_back();
                    continue;
                }

                if (!is_active()) continue;

                if (dir == "pragma") {
                    if (to_lower_copy(arg) == "once") {
                        m_pragma_once_files.insert(normalize_path_key(current_file));
                    }
                    continue;
                }
                if (dir == "define") {
                    parse_define(arg);
                    continue;
                }
                if (dir == "undef") {
                    std::string sym = trim_copy(arg);
                    size_t end = 0;
                    while (end < sym.size() && is_ident_char(sym[end])) ++end;
                    m_macros.erase(sym.substr(0, end));
                    continue;
                }
                if (dir == "include") {
                    std::string inc_text;
                    std::filesystem::path inc_path;
                    if (resolve_include(arg, current_file.parent_path(), inc_text, inc_path)) {
                        std::string key = normalize_path_key(inc_path);
                        if (m_pragma_once_files.find(key) == m_pragma_once_files.end()) {
                            out << run(inc_text, inc_path, depth + 1) << "\n";
                        }
                    } else {
                        m_warnings.push_back("Could not resolve #include " + arg);
                    }
                    continue;
                }
                if (dir == "error") {
                    m_warnings.push_back("#error " + arg);
                    continue;
                }
                continue;
            }

            if (!is_active()) continue;
            active_chunk << line << "\n";
        }
        flush_chunk();

        return out.str();
    }

private:
    std::filesystem::path m_base_dir;
    uint32_t m_buf_w = 1920;
    uint32_t m_buf_h = 1080;
    std::vector<std::string> &m_warnings;
    std::unordered_map<std::string, MacroDef> m_macros;
    std::vector<std::filesystem::path> m_search_dirs;
    std::unordered_set<std::string> m_pragma_once_files;

    static std::string normalize_path_key(const std::filesystem::path &p) {
        std::error_code ec;
        auto can = std::filesystem::weakly_canonical(p, ec);
        return to_lower_copy(ec ? p.string() : can.string());
    }

    bool is_defined_macro(const std::string &sym) const {
        if (sym == "BUFFER_WIDTH" || sym == "BUFFER_HEIGHT" ||
            sym == "BUFFER_RCP_WIDTH" || sym == "BUFFER_RCP_HEIGHT" ||
            sym == "BUFFER_PIXEL_SIZE" || sym == "BUFFER_SCREEN_SIZE" ||
            sym == "BUFFER_ASPECT_RATIO") {
            return true;
        }
        return m_macros.find(sym) != m_macros.end();
    }

    void build_include_search_dirs(const std::filesystem::path &base_file) {
        std::vector<std::filesystem::path> raw;
        if (!base_file.empty()) {
            std::filesystem::path d = base_file.parent_path();
            for (int k = 0; k < 4 && !d.empty(); ++k) {
                raw.push_back(d);
                raw.push_back(d / "Shaders");
                raw.push_back(d / "reshade-shaders" / "Shaders");
                raw.push_back(d / "include");
                d = d.parent_path();
            }
        }
        wchar_t exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
            std::filesystem::path ed = std::filesystem::path(exe_buf).parent_path();
            raw.push_back(ed / "reshade-shaders" / "Shaders");
            raw.push_back(ed / "Shaders");
            raw.push_back(ed.parent_path() / "reshade-shaders" / "Shaders");
            raw.push_back(ed.parent_path().parent_path() / "reshade-shaders" / "Shaders");
        }
        std::error_code ec;
        for (const auto &r : raw) {
            if (!r.empty() && std::filesystem::exists(r, ec) && std::filesystem::is_directory(r, ec)) {
                m_search_dirs.push_back(r);
            }
        }
    }

    bool resolve_include(
        const std::string &arg,
        const std::filesystem::path &cur_dir,
        std::string &out_content,
        std::filesystem::path &out_path) const
    {
        std::string clean;
        size_t q1 = arg.find('"');
        size_t q2 = (q1 != std::string::npos) ? arg.find('"', q1 + 1) : std::string::npos;
        if (q1 != std::string::npos && q2 != std::string::npos) {
            clean = arg.substr(q1 + 1, q2 - q1 - 1);
        } else {
            size_t a1 = arg.find('<');
            size_t a2 = (a1 != std::string::npos) ? arg.find('>', a1 + 1) : std::string::npos;
            if (a1 != std::string::npos && a2 != std::string::npos) {
                clean = arg.substr(a1 + 1, a2 - a1 - 1);
            }
        }
        if (clean.empty()) return false;

        std::vector<std::filesystem::path> tries;
        if (!cur_dir.empty()) tries.push_back(cur_dir / clean);
        if (!m_base_dir.empty()) tries.push_back(m_base_dir / clean);
        for (const auto &sd : m_search_dirs) {
            tries.push_back(sd / clean);
        }

        std::error_code ec;
        for (const auto &t : tries) {
            if (std::filesystem::exists(t, ec) && std::filesystem::is_regular_file(t, ec)) {
                std::ifstream in(t, std::ios::binary);
                if (in.is_open()) {
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    out_content = ss.str();
                    out_path = t;
                    return true;
                }
            }
        }

        std::string fname = to_lower_copy(std::filesystem::path(clean).filename().string());
        if (fname == "reshadeui.fxh") {
            out_content = kFallbackReShadeUiFxh;
            out_path = "__builtin__/ReShadeUI.fxh";
            return true;
        }
        if (fname == "reshade.fxh") {
            out_content = kFallbackReShadeFxh;
            out_path = "__builtin__/ReShade.fxh";
            return true;
        }
        return false;
    }

    void parse_define(const std::string &arg) {
        size_t i = 0;
        while (i < arg.size() && is_ident_char(arg[i])) ++i;
        if (i == 0) return;
        std::string name = arg.substr(0, i);
        // Never let shader overwrite runtime dimension globals if we treat them dynamically
        MacroDef def;
        if (i < arg.size() && arg[i] == '(') {
            size_t close_p = arg.find(')', i + 1);
            if (close_p != std::string::npos) {
                def.is_func = true;
                std::string plist = arg.substr(i + 1, close_p - i - 1);
                for (const auto &p : split_top_level_args(plist)) {
                    std::string tp = trim_copy(p);
                    if (tp == "...") {
                        def.variadic = true;
                    } else if (!tp.empty()) {
                        def.params.push_back(tp);
                    }
                }
                def.body = trim_copy(arg.substr(close_p + 1));
            }
        } else {
            def.body = trim_copy(arg.substr(i));
        }
        m_macros[name] = std::move(def);
    }

    std::string expand_macros(const std::string &input, std::unordered_set<std::string> &disabled, int depth = 0) const {
        if (depth > 16 || m_macros.empty()) return input;
        std::string out;
        out.reserve(input.size());
        size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '"') {
                out.push_back(input[i++]);
                while (i < input.size()) {
                    char c = input[i++];
                    out.push_back(c);
                    if (c == '\\' && i < input.size()) out.push_back(input[i++]);
                    else if (c == '"') break;
                }
                continue;
            }
            if (is_ident_start(input[i])) {
                size_t start = i++;
                while (i < input.size() && is_ident_char(input[i])) ++i;
                std::string ident = input.substr(start, i - start);

                auto it = m_macros.find(ident);
                if (it != m_macros.end() && disabled.find(ident) == disabled.end()) {
                    const MacroDef &m = it->second;
                    if (!m.is_func) {
                        disabled.insert(ident);
                        std::string sub = expand_macros(m.body, disabled, depth + 1);
                        disabled.erase(ident);
                        out += sub;
                        continue;
                    }
                    size_t ws = i;
                    while (ws < input.size() && std::isspace(static_cast<unsigned char>(input[ws]))) ++ws;
                    if (ws < input.size() && input[ws] == '(') {
                        size_t close_p = find_matching_delim(input, ws, '(', ')');
                        if (close_p != std::string::npos) {
                            std::string raw_args = input.substr(ws + 1, close_p - ws - 1);
                            std::vector<std::string> args = raw_args.empty() && m.params.empty()
                                ? std::vector<std::string>{}
                                : split_top_level_args(raw_args);
                            std::unordered_map<std::string, std::string> pmap;
                            for (size_t k = 0; k < m.params.size(); ++k) {
                                std::string val = (k < args.size()) ? args[k] : std::string();
                                pmap[m.params[k]] = expand_macros(val, disabled, depth + 1);
                            }
                            std::string var_arg;
                            if (m.variadic && args.size() > m.params.size()) {
                                for (size_t k = m.params.size(); k < args.size(); ++k) {
                                    if (!var_arg.empty()) var_arg += ", ";
                                    var_arg += expand_macros(args[k], disabled, depth + 1);
                                }
                            }
                            pmap["__VA_ARGS__"] = var_arg;

                            std::string inst = substitute_params(m.body, pmap);
                            disabled.insert(ident);
                            std::string sub = expand_macros(inst, disabled, depth + 1);
                            disabled.erase(ident);
                            out += sub;
                            i = close_p + 1;
                            continue;
                        }
                    }
                }
                out += ident;
            } else {
                out.push_back(input[i++]);
            }
        }
        return out;
    }

    static std::string substitute_params(const std::string &body, const std::unordered_map<std::string, std::string> &pmap) {
        std::string res;
        res.reserve(body.size());
        size_t i = 0;
        while (i < body.size()) {
            if (body[i] == '"') {
                res.push_back(body[i++]);
                while (i < body.size()) {
                    char c = body[i++];
                    res.push_back(c);
                    if (c == '\\' && i < body.size()) res.push_back(body[i++]);
                    else if (c == '"') break;
                }
                continue;
            }
            if (i + 1 < body.size() && body[i] == '#' && body[i + 1] == '#') {
                while (!res.empty() && (res.back() == ' ' || res.back() == '\t')) res.pop_back();
                i += 2;
                while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
                continue;
            }
            if (is_ident_start(body[i])) {
                size_t st = i++;
                while (i < body.size() && is_ident_char(body[i])) ++i;
                std::string id = body.substr(st, i - st);
                auto it = pmap.find(id);
                if (it != pmap.end()) res += it->second;
                else res += id;
            } else {
                res.push_back(body[i++]);
            }
        }
        return res;
    }

    // Integer expression evaluator for #if / #elif
    bool eval_pp_condition(const std::string &expr) const {
        // Resolve defined(X) / defined X before macro expansion
        std::string pre;
        size_t i = 0;
        while (i < expr.size()) {
            if (starts_with_word(expr, i, "defined")) {
                i += 7;
                while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
                bool paren = false;
                if (i < expr.size() && expr[i] == '(') { paren = true; ++i; }
                while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
                size_t st = i;
                while (i < expr.size() && is_ident_char(expr[i])) ++i;
                std::string sym = expr.substr(st, i - st);
                if (paren) {
                    while (i < expr.size() && expr[i] != ')') ++i;
                    if (i < expr.size()) ++i;
                }
                pre += is_defined_macro(sym) ? "1" : "0";
            } else {
                pre.push_back(expr[i++]);
            }
        }

        // Expand active macros & substitute BUFFER_WIDTH / BUFFER_HEIGHT constants
        Preprocessor temp_pp = *this;
        temp_pp.define_simple("BUFFER_WIDTH", std::to_string(m_buf_w));
        temp_pp.define_simple("BUFFER_HEIGHT", std::to_string(m_buf_h));
        std::unordered_set<std::string> disabled;
        std::string expanded = temp_pp.expand_macros(pre, disabled);

        size_t pos = 0;
        return parse_pp_ternary(expanded, pos) != 0;
    }

    static void skip_spaces(const std::string &s, size_t &p) {
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    }

    static long long parse_pp_ternary(const std::string &s, size_t &p) {
        long long cond = parse_pp_or(s, p);
        skip_spaces(s, p);
        if (p < s.size() && s[p] == '?') {
            ++p;
            long long a = parse_pp_ternary(s, p);
            skip_spaces(s, p);
            if (p < s.size() && s[p] == ':') ++p;
            long long b = parse_pp_ternary(s, p);
            return cond ? a : b;
        }
        return cond;
    }

    static long long parse_pp_or(const std::string &s, size_t &p) {
        long long v = parse_pp_and(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p + 1 < s.size() && s[p] == '|' && s[p + 1] == '|') {
                p += 2;
                long long r = parse_pp_and(s, p);
                v = (v != 0 || r != 0) ? 1 : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_and(const std::string &s, size_t &p) {
        long long v = parse_pp_bitor(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p + 1 < s.size() && s[p] == '&' && s[p + 1] == '&') {
                p += 2;
                long long r = parse_pp_bitor(s, p);
                v = (v != 0 && r != 0) ? 1 : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_bitor(const std::string &s, size_t &p) {
        long long v = parse_pp_bitxor(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p < s.size() && s[p] == '|' && (p + 1 >= s.size() || s[p + 1] != '|')) {
                ++p;
                v |= parse_pp_bitxor(s, p);
            } else break;
        }
        return v;
    }

    static long long parse_pp_bitxor(const std::string &s, size_t &p) {
        long long v = parse_pp_bitand(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p < s.size() && s[p] == '^') {
                ++p;
                v ^= parse_pp_bitand(s, p);
            } else break;
        }
        return v;
    }

    static long long parse_pp_bitand(const std::string &s, size_t &p) {
        long long v = parse_pp_eq(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p < s.size() && s[p] == '&' && (p + 1 >= s.size() || s[p + 1] != '&')) {
                ++p;
                v &= parse_pp_eq(s, p);
            } else break;
        }
        return v;
    }

    static long long parse_pp_eq(const std::string &s, size_t &p) {
        long long v = parse_pp_rel(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p + 1 < s.size() && s[p] == '=' && s[p + 1] == '=') {
                p += 2;
                v = (v == parse_pp_rel(s, p)) ? 1 : 0;
            } else if (p + 1 < s.size() && s[p] == '!' && s[p + 1] == '=') {
                p += 2;
                v = (v != parse_pp_rel(s, p)) ? 1 : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_rel(const std::string &s, size_t &p) {
        long long v = parse_pp_shift(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p + 1 < s.size() && s[p] == '<' && s[p + 1] == '=') {
                p += 2;
                v = (v <= parse_pp_shift(s, p)) ? 1 : 0;
            } else if (p + 1 < s.size() && s[p] == '>' && s[p + 1] == '=') {
                p += 2;
                v = (v >= parse_pp_shift(s, p)) ? 1 : 0;
            } else if (p < s.size() && s[p] == '<' && (p + 1 >= s.size() || s[p + 1] != '<')) {
                ++p;
                v = (v < parse_pp_shift(s, p)) ? 1 : 0;
            } else if (p < s.size() && s[p] == '>' && (p + 1 >= s.size() || s[p + 1] != '>')) {
                ++p;
                v = (v > parse_pp_shift(s, p)) ? 1 : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_shift(const std::string &s, size_t &p) {
        long long v = parse_pp_add(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p + 1 < s.size() && s[p] == '<' && s[p + 1] == '<') {
                p += 2;
                long long r = parse_pp_add(s, p);
                v = (r >= 0 && r < 63) ? (v << r) : 0;
            } else if (p + 1 < s.size() && s[p] == '>' && s[p + 1] == '>') {
                p += 2;
                long long r = parse_pp_add(s, p);
                v = (r >= 0 && r < 63) ? (v >> r) : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_add(const std::string &s, size_t &p) {
        long long v = parse_pp_mul(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p < s.size() && s[p] == '+') { ++p; v += parse_pp_mul(s, p); }
            else if (p < s.size() && s[p] == '-') { ++p; v -= parse_pp_mul(s, p); }
            else break;
        }
        return v;
    }

    static long long parse_pp_mul(const std::string &s, size_t &p) {
        long long v = parse_pp_unary(s, p);
        while (true) {
            skip_spaces(s, p);
            if (p < s.size() && s[p] == '*') { ++p; v *= parse_pp_unary(s, p); }
            else if (p < s.size() && s[p] == '/') {
                ++p;
                long long r = parse_pp_unary(s, p);
                v = (r != 0) ? (v / r) : 0;
            } else if (p < s.size() && s[p] == '%') {
                ++p;
                long long r = parse_pp_unary(s, p);
                v = (r != 0) ? (v % r) : 0;
            } else break;
        }
        return v;
    }

    static long long parse_pp_unary(const std::string &s, size_t &p) {
        skip_spaces(s, p);
        if (p < s.size() && s[p] == '!') { ++p; return parse_pp_unary(s, p) ? 0 : 1; }
        if (p < s.size() && s[p] == '~') { ++p; return ~parse_pp_unary(s, p); }
        if (p < s.size() && s[p] == '+') { ++p; return parse_pp_unary(s, p); }
        if (p < s.size() && s[p] == '-') { ++p; return -parse_pp_unary(s, p); }
        return parse_pp_primary(s, p);
    }

    static long long parse_pp_primary(const std::string &s, size_t &p) {
        skip_spaces(s, p);
        if (p < s.size() && s[p] == '(') {
            ++p;
            long long v = parse_pp_ternary(s, p);
            skip_spaces(s, p);
            if (p < s.size() && s[p] == ')') ++p;
            return v;
        }
        if (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            char *endp = nullptr;
            long long v = std::strtoll(s.c_str() + p, &endp, 0);
            p = static_cast<size_t>(endp - s.c_str());
            while (p < s.size() && (s[p] == 'u' || s[p] == 'U' || s[p] == 'l' || s[p] == 'L')) ++p;
            return v;
        }
        if (p < s.size() && is_ident_start(s[p])) {
            size_t st = p++;
            while (p < s.size() && is_ident_char(s[p])) ++p;
            std::string sym = s.substr(st, p - st);
            if (sym == "true") return 1;
            return 0;
        }
        return 0;
    }
};

// -------------------- Parsing & Transpiling Helpers --------------------

double eval_dim_expr(const std::string &expr_in, uint32_t buf_w, uint32_t buf_h) {
    // Simple arithmetic parser for Width/Height = BUFFER_WIDTH / 2 etc.
    std::string expr = expr_in;
    auto replace_tok = [&](const std::string &from, const std::string &to) {
        size_t pos = 0;
        while ((pos = expr.find(from, pos)) != std::string::npos) {
            bool left_ok = (pos == 0 || !is_ident_char(expr[pos - 1]));
            bool right_ok = (pos + from.size() >= expr.size() || !is_ident_char(expr[pos + from.size()]));
            if (left_ok && right_ok) {
                expr.replace(pos, from.size(), to);
                pos += to.size();
            } else {
                pos += from.size();
            }
        }
    };
    replace_tok("BUFFER_WIDTH", std::to_string(buf_w));
    replace_tok("BUFFER_HEIGHT", std::to_string(buf_h));
    replace_tok("BUFFER_RCP_WIDTH", std::to_string(1.0 / std::max(1u, buf_w)));
    replace_tok("BUFFER_RCP_HEIGHT", std::to_string(1.0 / std::max(1u, buf_h)));

    size_t p = 0;
    auto parse_add = [&](auto &self) -> double {
        auto parse_mul = [&](auto &self_m) -> double {
            auto parse_atom = [&](auto &self_a) -> double {
                while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
                if (p < expr.size() && expr[p] == '(') {
                    ++p;
                    double v = self(self);
                    while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
                    if (p < expr.size() && expr[p] == ')') ++p;
                    return v;
                }
                if (p < expr.size() && expr[p] == '-') {
                    ++p;
                    return -self_a(self_a);
                }
                char *endp = nullptr;
                double v = std::strtod(expr.c_str() + p, &endp);
                if (endp == expr.c_str() + p) return 0.0;
                p = static_cast<size_t>(endp - expr.c_str());
                return v;
            };
            double v = parse_atom(parse_atom);
            while (true) {
                while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
                if (p < expr.size() && expr[p] == '*') { ++p; v *= parse_atom(parse_atom); }
                else if (p < expr.size() && expr[p] == '/') {
                    ++p;
                    double r = parse_atom(parse_atom);
                    v = (std::abs(r) > 1e-9) ? (v / r) : v;
                } else break;
            }
            return v;
        };
        double v = parse_mul(parse_mul);
        while (true) {
            while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
            if (p < expr.size() && expr[p] == '+') { ++p; v += parse_mul(parse_mul); }
            else if (p < expr.size() && expr[p] == '-') { ++p; v -= parse_mul(parse_mul); }
            else break;
        }
        return v;
    };
    return parse_add(parse_add);
}

DXGI_FORMAT parse_reshade_format(const std::string &fmt_in) {
    std::string f = to_lower_copy(trim_copy(fmt_in));
    if (f == "r8") return DXGI_FORMAT_R8_UNORM;
    if (f == "r16") return DXGI_FORMAT_R16_UNORM;
    if (f == "r16f") return DXGI_FORMAT_R16_FLOAT;
    if (f == "r32f") return DXGI_FORMAT_R32_FLOAT;
    if (f == "rg8") return DXGI_FORMAT_R8G8_UNORM;
    if (f == "rg16") return DXGI_FORMAT_R16G16_UNORM;
    if (f == "rg16f") return DXGI_FORMAT_R16G16_FLOAT;
    if (f == "rg32f") return DXGI_FORMAT_R32G32_FLOAT;
    if (f == "rgba8" || f == "rgb8") return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == "rgba16") return DXGI_FORMAT_R16G16B16A16_UNORM;
    if (f == "rgba16f") return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == "rgba32f") return DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (f == "rgb10a2") return DXGI_FORMAT_R10G10B10A2_UNORM;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

std::unordered_map<std::string, std::string> parse_key_value_block(const std::string &body, bool decode_strings_keep_nul = true) {
    std::unordered_map<std::string, std::string> kv;
    size_t i = 0;
    while (i < body.size()) {
        i = skip_ws_and_comments(body, i);
        if (i >= body.size()) break;
        size_t k_start = i;
        while (i < body.size() && (is_ident_char(body[i]))) ++i;
        std::string key = body.substr(k_start, i - k_start);
        if (key.empty()) { ++i; continue; }

        i = skip_ws_and_comments(body, i);
        if (i >= body.size() || body[i] != '=') {
            while (i < body.size() && body[i] != ';') ++i;
            if (i < body.size()) ++i;
            continue;
        }
        ++i;
        i = skip_ws_and_comments(body, i);
        size_t v_start = i;
        int paren = 0;
        while (i < body.size()) {
            char c = body[i];
            if (c == '"') {
                ++i;
                while (i < body.size()) {
                    if (body[i] == '\\' && i + 1 < body.size()) i += 2;
                    else if (body[i++] == '"') break;
                }
                continue;
            }
            if (c == '(') ++paren;
            else if (c == ')' && paren > 0) --paren;
            else if (c == ';' && paren == 0) break;
            ++i;
        }
        std::string raw_val = trim_copy(body.substr(v_start, i - v_start));
        if (i < body.size() && body[i] == ';') ++i;
        if (!raw_val.empty() && raw_val.front() == '"') {
            raw_val = decode_escaped_string_sequence(raw_val, decode_strings_keep_nul);
        }
        kv[to_lower_copy(key)] = raw_val;
    }
    return kv;
}

bool parse_float_list(const std::string &expr, float *out_vals, int max_n, int &out_count) {
    out_count = 0;
    std::string s = trim_copy(expr);
    size_t lp = s.find('(');
    size_t rp = s.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
        s = s.substr(lp + 1, rp - lp - 1);
    } else if (!s.empty() && s.front() == '{' && s.back() == '}') {
        s = s.substr(1, s.size() - 2);
    }
    auto parts = split_top_level_args(s);
    for (const auto &part : parts) {
        if (out_count >= max_n) break;
        std::string tp = trim_copy(part);
        if (tp.empty()) continue;
        if (to_lower_copy(tp) == "true") {
            out_vals[out_count++] = 1.0f;
            continue;
        }
        if (to_lower_copy(tp) == "false") {
            out_vals[out_count++] = 0.0f;
            continue;
        }
        char *endp = nullptr;
        double v = std::strtod(tp.c_str(), &endp);
        if (endp == tp.c_str()) return false;
        out_vals[out_count++] = static_cast<float>(v);
    }
    return out_count > 0;
}

SystemUniformSemantic detect_system_semantic(const std::string &source_val) {
    std::string s = to_lower_copy(trim_copy(source_val));
    if (s == "frametime") return SystemUniformSemantic::FrameTimeMs;
    if (s == "framecount") return SystemUniformSemantic::FrameCount;
    if (s == "timer") return SystemUniformSemantic::TimerMs;
    if (s == "buffer_size" || s == "buffersize" || s == "screensize") return SystemUniformSemantic::BufferSize;
    if (s == "pixelsize" || s == "pixel_size") return SystemUniformSemantic::PixelSize;
    return SystemUniformSemantic::None;
}

struct ParsedUniformInfo {
    std::string hlsl_type;
    std::string name;
    int ui_uniform_index = -1;
    SystemUniformSemantic system_semantic = SystemUniformSemantic::None;
};

struct ParsedTextureDecl {
    std::string name;
    std::string semantic;
    CustomRenderTargetSpec rt_spec;
    bool is_backbuffer = false;
    bool is_depth = false;
};

struct ParsedSamplerDecl {
    std::string name;
    std::string tex_name;
    int filter_mode = 0;
    int address_u = 0;
    int address_v = 0;
};

struct ParsedPassDecl {
    std::string pass_name;
    std::string vs_entry = "PostProcessVS";
    std::string ps_entry;
    std::vector<std::string> render_targets;
    bool clear_rt = false;
    bool srgb_write = false;
};

struct ParsedShaderProgram {
    std::string hlsl_body;
    std::vector<ParsedTextureDecl> textures;
    std::vector<ParsedSamplerDecl> samplers;
    std::vector<ParsedUniformInfo> uniforms_meta;
    std::vector<ParsedPassDecl> passes;
};

// Flatten `namespace X { ... }` and replace `X::member` -> `X_member`
std::string flatten_namespaces(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    std::vector<std::string> ns_stack;

    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '"') {
            out.push_back(src[i++]);
            while (i < src.size()) {
                char c = src[i++];
                out.push_back(c);
                if (c == '\\' && i < src.size()) out.push_back(src[i++]);
                else if (c == '"') break;
            }
            continue;
        }
        if (starts_with_word(src, i, "namespace")) {
            size_t j = skip_ws_and_comments(src, i + 9);
            size_t n_start = j;
            while (j < src.size() && is_ident_char(src[j])) ++j;
            std::string ns_name = src.substr(n_start, j - n_start);
            j = skip_ws_and_comments(src, j);
            if (!ns_name.empty() && j < src.size() && src[j] == '{') {
                size_t close_b = find_matching_delim(src, j, '{', '}');
                if (close_b != std::string::npos) {
                    std::string inner = src.substr(j + 1, close_b - j - 1);
                    // prefix top-level declarations inside inner with ns_name_
                    std::string inner_flat = flatten_namespaces(inner);
                    // Replace qualified references inside same namespace (or leave global ones)
                    // Rename declared symbols in namespace body cleanly
                    out += "\n// [namespace " + ns_name + "]\n";
                    // Extract top-level declared symbols in inner_flat so calls inside inner_flat also see prefix
                    std::vector<std::string> ns_symbols;
                    {
                        size_t p = 0;
                        int bdepth = 0;
                        while (p < inner_flat.size()) {
                            p = skip_ws_and_comments(inner_flat, p);
                            if (p >= inner_flat.size()) break;
                            if (inner_flat[p] == '{') { ++bdepth; ++p; continue; }
                            if (inner_flat[p] == '}') { if (bdepth > 0) --bdepth; ++p; continue; }
                            if (bdepth == 0) {
                                // Scan statement/declaration head up to ';', '{', or '='
                                size_t head_end = p;
                                int pd = 0, ad = 0;
                                while (head_end < inner_flat.size()) {
                                    char ch = inner_flat[head_end];
                                    if (ch == '(') ++pd;
                                    else if (ch == ')' && pd > 0) --pd;
                                    else if (ch == '<' && pd == 0) ++ad;
                                    else if (ch == '>' && ad > 0 && pd == 0) --ad;
                                    else if ((ch == ';' || ch == '{' || ch == '=') && pd == 0 && ad == 0) break;
                                    else if (ch == '(' && pd == 1) {
                                        // function name is identifier right before this '('
                                    }
                                    ++head_end;
                                }
                                // Find identifier right before '(', '=', '<', ':', ';', '{'
                                std::string head = inner_flat.substr(p, head_end - p);
                                size_t paren_pos = head.find('(');
                                std::string name_search = (paren_pos != std::string::npos) ? head.substr(0, paren_pos) : head;
                                // Strip semantic `: ...`, annotations `<...>`, or trailing array bounds `[...]`
                                size_t colon_pos = name_search.find(':');
                                if (colon_pos != std::string::npos) name_search = name_search.substr(0, colon_pos);
                                size_t ang_pos = name_search.find('<');
                                if (ang_pos != std::string::npos) name_search = name_search.substr(0, ang_pos);
                                size_t brk_pos = name_search.find('[');
                                if (brk_pos != std::string::npos) name_search = name_search.substr(0, brk_pos);
                                size_t k = name_search.size();
                                while (k > 0 && !is_ident_char(name_search[k - 1])) --k;
                                size_t end_id = k;
                                while (k > 0 && is_ident_char(name_search[k - 1])) --k;
                                if (end_id > k) {
                                    std::string sym = name_search.substr(k, end_id - k);
                                    if (!sym.empty() && sym != "struct" && sym != "return" && sym != "else") {
                                        ns_symbols.push_back(sym);
                                    }
                                }
                                p = head_end;
                                if (p < inner_flat.size()) {
                                    if (inner_flat[p] == '{') {
                                        size_t cb = find_matching_delim(inner_flat, p, '{', '}');
                                        p = (cb != std::string::npos) ? cb + 1 : inner_flat.size();
                                    } else {
                                        while (p < inner_flat.size() && inner_flat[p] != ';' && inner_flat[p] != '{') ++p;
                                        if (p < inner_flat.size() && inner_flat[p] == '{') {
                                            size_t cb = find_matching_delim(inner_flat, p, '{', '}');
                                            p = (cb != std::string::npos) ? cb + 1 : inner_flat.size();
                                        } else if (p < inner_flat.size()) {
                                            ++p;
                                        }
                                    }
                                }
                            } else {
                                ++p;
                            }
                        }
                    }

                    // Prefix occurrences of namespace-local symbols inside inner_flat
                    std::unordered_set<std::string> sym_set(ns_symbols.begin(), ns_symbols.end());
                    std::string prefixed;
                    prefixed.reserve(inner_flat.size());
                    for (size_t q = 0; q < inner_flat.size();) {
                        if (inner_flat[q] == '"') {
                            prefixed.push_back(inner_flat[q++]);
                            while (q < inner_flat.size()) {
                                char ch = inner_flat[q++];
                                prefixed.push_back(ch);
                                if (ch == '\\' && q < inner_flat.size()) prefixed.push_back(inner_flat[q++]);
                                else if (ch == '"') break;
                            }
                        } else if (is_ident_start(inner_flat[q])) {
                            size_t st = q++;
                            while (q < inner_flat.size() && is_ident_char(inner_flat[q])) ++q;
                            std::string tok = inner_flat.substr(st, q - st);
                            // Don't double prefix or rename member access `.sym`
                            bool is_member = (st > 0 && inner_flat[st - 1] == '.');
                            if (!is_member && sym_set.count(tok)) {
                                prefixed += ns_name + "_" + tok;
                            } else {
                                prefixed += tok;
                            }
                        } else {
                            prefixed.push_back(inner_flat[q++]);
                        }
                    }
                    out += prefixed;
                    out += "\n";
                    i = close_b + 1;
                    continue;
                }
            }
        }

        // Rewrite Foo::bar -> Foo_bar outside strings
        if (i + 1 < src.size() && src[i] == ':' && src[i + 1] == ':') {
            out.push_back('_');
            i += 2;
            continue;
        }
        out.push_back(src[i++]);
    }
    return out;
}

std::string rewrite_tex2d_offset_and_gather(
    const std::string &code,
    const std::unordered_map<std::string, std::string> &sampler_to_tex)
{
    // Rewrites tex2Doffset(S, uv, off) -> __sl_tex_<T>.Sample(__sl_smp_<S>, (uv), (off))
    // preserving compile-time constant integer literal offsets required by FXC/D3DCompile.
    // Also rewrites tex2DgatherR/G/B/A(S, uv, ...) if encountered.
    struct IntrMap { const char *name; const char *method; };
    static constexpr IntrMap kGather[] = {
        { "tex2DgatherR", "GatherRed" },
        { "tex2DgatherG", "GatherGreen" },
        { "tex2DgatherB", "GatherBlue" },
        { "tex2DgatherA", "GatherAlpha" },
    };

    std::string out;
    out.reserve(code.size());
    size_t i = 0;
    while (i < code.size()) {
        if (code[i] == '"') {
            out.push_back(code[i++]);
            while (i < code.size()) {
                char c = code[i++];
                out.push_back(c);
                if (c == '\\' && i < code.size()) out.push_back(code[i++]);
                else if (c == '"') break;
            }
            continue;
        }

        if (starts_with_word(code, i, "tex2Doffset")) {
            size_t lp = skip_ws_and_comments(code, i + 11);
            if (lp < code.size() && code[lp] == '(') {
                size_t rp = find_matching_delim(code, lp, '(', ')');
                if (rp != std::string::npos) {
                    auto args = split_top_level_args(code.substr(lp + 1, rp - lp - 1));
                    if (args.size() >= 3) {
                        std::string s_expr = trim_copy(args[0]);
                        auto it = sampler_to_tex.find(s_expr);
                        if (it != sampler_to_tex.end()) {
                            out += "(__sl_tex_" + it->second + ".Sample(__sl_smp_" + s_expr + ", (" +
                                   args[1] + "), (" + args[2] + ")))";
                            i = rp + 1;
                            continue;
                        }
                        // Fallback if sampler was renamed parameter `__sl_p_smp`
                        out += "(__p_tex_" + s_expr + ".Sample(__p_smp_" + s_expr + ", (" +
                               args[1] + "), (" + args[2] + ")))";
                        i = rp + 1;
                        continue;
                    }
                }
            }
        }

        bool matched_gather = false;
        for (const auto &g : kGather) {
            if (starts_with_word(code, i, g.name)) {
                size_t lp = skip_ws_and_comments(code, i + std::strlen(g.name));
                if (lp < code.size() && code[lp] == '(') {
                    size_t rp = find_matching_delim(code, lp, '(', ')');
                    if (rp != std::string::npos) {
                        auto args = split_top_level_args(code.substr(lp + 1, rp - lp - 1));
                        if (args.size() >= 2) {
                            std::string s_expr = trim_copy(args[0]);
                            auto it = sampler_to_tex.find(s_expr);
                            if (it != sampler_to_tex.end()) {
                                out += "(__sl_tex_" + it->second + "." + g.method + "(__sl_smp_" + s_expr + ", (" + args[1] + ")";
                                if (args.size() >= 3) out += ", (" + args[2] + ")";
                                out += "))";
                                i = rp + 1;
                                matched_gather = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (matched_gather) continue;

        out.push_back(code[i++]);
    }
    return out;
}

std::string rewrite_sampler_function_params(const std::string &code) {
    // Rewrites helper parameters `(..., sampler2D foo, ...)` -> `(..., Texture2D __p_tex_foo, SamplerState __p_smp_foo, ...)`
    // and transforms tex2D(foo, ...) inside that function to work cleanly.
    std::string out;
    out.reserve(code.size());
    size_t i = 0;
    while (i < code.size()) {
        if (code[i] == '"') {
            out.push_back(code[i++]);
            while (i < code.size()) {
                char c = code[i++];
                out.push_back(c);
                if (c == '\\' && i < code.size()) out.push_back(code[i++]);
                else if (c == '"') break;
            }
            continue;
        }
        bool is_s2d = starts_with_word(code, i, "sampler2D");
        bool is_s = !is_s2d && starts_with_word(code, i, "sampler");
        if (is_s2d || is_s) {
            size_t j = skip_ws_and_comments(code, i + (is_s2d ? 9 : 7));
            size_t ns = j;
            while (j < code.size() && is_ident_char(code[j])) ++j;
            std::string pname = code.substr(ns, j - ns);
            size_t after = skip_ws_and_comments(code, j);
            if (!pname.empty() && after < code.size() && (code[after] == ',' || code[after] == ')')) {
                out += "Texture2D __p_tex_" + pname + ", SamplerState __p_smp_" + pname;
                i = j;
                continue;
            }
        }
        out.push_back(code[i++]);
    }
    return out;
}

std::string build_reshade_preamble(
    const ParsedShaderProgram &prog,
    uint32_t main_w,
    uint32_t main_h)
{
    std::ostringstream ss;
    ss << "// ShaderLab Auto-Generated HLSL SM 5.0 Preamble\n";
    ss << "float4 __sl_system_viewport;\n"; // xy=(w, h), zw=(1/w, 1/h)
    ss << "float4 __sl_system_time;\n";     // x=frametime_ms, y=timer_ms, z=framecount_f, w=time_sec
    ss << "int __sl_framecount_i;\n";
    ss << "#undef BUFFER_WIDTH\n";
    ss << "#undef BUFFER_HEIGHT\n";
    ss << "#undef BUFFER_RCP_WIDTH\n";
    ss << "#undef BUFFER_RCP_HEIGHT\n";
    ss << "#undef BUFFER_PIXEL_SIZE\n";
    ss << "#undef BUFFER_SCREEN_SIZE\n";
    ss << "#undef BUFFER_ASPECT_RATIO\n";
    ss << "#define BUFFER_WIDTH (__sl_system_viewport.x)\n";
    ss << "#define BUFFER_HEIGHT (__sl_system_viewport.y)\n";
    ss << "#define BUFFER_RCP_WIDTH (__sl_system_viewport.z)\n";
    ss << "#define BUFFER_RCP_HEIGHT (__sl_system_viewport.w)\n";
    ss << "#define BUFFER_PIXEL_SIZE (float2(__sl_system_viewport.z, __sl_system_viewport.w))\n";
    ss << "#define BUFFER_SCREEN_SIZE (float2(__sl_system_viewport.x, __sl_system_viewport.y))\n";
    ss << "#define BUFFER_ASPECT_RATIO (__sl_system_viewport.x * __sl_system_viewport.w)\n";

    std::unordered_set<std::string> emitted_tex;
    for (const auto &t : prog.textures) {
        if (emitted_tex.insert(t.name).second) {
            ss << "Texture2D __sl_tex_" << t.name << ";\n";
        }
    }
    if (emitted_tex.find("BackBufferTex") == emitted_tex.end()) {
        ss << "Texture2D __sl_tex_BackBufferTex;\n";
    }
    if (emitted_tex.find("DepthBufferTex") == emitted_tex.end()) {
        ss << "Texture2D __sl_tex_DepthBufferTex;\n";
    }

    for (const auto &s : prog.samplers) {
        ss << "SamplerState __sl_smp_" << s.name << ";\n";
        std::string tex_ref = s.tex_name.empty() ? "BackBufferTex" : s.tex_name;
        ss << "#define " << s.name << " __sl_tex_" << tex_ref << ", __sl_smp_" << s.name << "\n";
    }

    ss << R"HLSL(
float4 __sl_tex2D(Texture2D t, SamplerState s, float2 uv) { return t.Sample(s, uv); }
float4 __sl_tex2D(Texture2D t, SamplerState s, float2 uv, float2 ddx_v, float2 ddy_v) { return t.SampleGrad(s, uv, ddx_v, ddy_v); }
float4 __sl_tex2Dlod(Texture2D t, SamplerState s, float4 uv_lod) { return t.SampleLevel(s, uv_lod.xy, uv_lod.w); }
float4 __sl_tex2Dbias(Texture2D t, SamplerState s, float4 uv_bias) { return t.SampleBias(s, uv_bias.xy, uv_bias.w); }
float4 __sl_tex2Dproj(Texture2D t, SamplerState s, float4 uv_proj) { return t.Sample(s, uv_proj.xy / uv_proj.w); }
float4 __sl_tex2Dfetch(Texture2D t, SamplerState s, int2 coord) { return t.Load(int3(coord, 0)); }
float4 __sl_tex2Dfetch(Texture2D t, SamplerState s, int2 coord, int mip) { return t.Load(int3(coord, mip)); }
float4 __sl_tex2Dfetch(Texture2D t, SamplerState s, int4 coord) { return t.Load(coord.xyw); }
int2 __sl_tex2Dsize(Texture2D t, SamplerState s) { uint w = 0, h = 0, m = 0; t.GetDimensions(0, w, h, m); return int2(w, h); }
int2 __sl_tex2Dsize(Texture2D t, SamplerState s, int mip) { uint w = 0, h = 0, m = 0; t.GetDimensions((uint)mip, w, h, m); return int2(w, h); }
#define tex2D __sl_tex2D
#define tex2Dlod __sl_tex2Dlod
#define tex2Dbias __sl_tex2Dbias
#define tex2Dproj __sl_tex2Dproj
#define tex2Dfetch __sl_tex2Dfetch
#define tex2Dsize __sl_tex2Dsize
)HLSL";
    (void)main_w;
    (void)main_h;
    return ss.str();
}

void parse_uniform_declaration(
    const std::string &stmt,
    gui::ShaderLabEffect &out_effect,
    std::vector<ParsedUniformInfo> &out_meta,
    std::ostringstream &out_hlsl)
{
    // Examples:
    // uniform float curve_height < ui_type = "slider"; ui_min = 0.01; ui_max = 2.0; > = 0.7;
    // uniform float frametime < source = "frametime"; >;
    size_t i = skip_ws_and_comments(stmt, 0);
    if (!starts_with_word(stmt, i, "uniform")) return;
    i = skip_ws_and_comments(stmt, i + 7);

    size_t t_st = i;
    while (i < stmt.size() && is_ident_char(stmt[i])) ++i;
    std::string var_type = stmt.substr(t_st, i - t_st);
    i = skip_ws_and_comments(stmt, i);

    size_t n_st = i;
    while (i < stmt.size() && is_ident_char(stmt[i])) ++i;
    std::string var_name = stmt.substr(n_st, i - n_st);
    if (var_type.empty() || var_name.empty()) return;

    i = skip_ws_and_comments(stmt, i);
    std::unordered_map<std::string, std::string> ann;
    if (i < stmt.size() && stmt[i] == '<') {
        size_t close_a = find_matching_angle(stmt, i);
        if (close_a != std::string::npos) {
            ann = parse_key_value_block(stmt.substr(i + 1, close_a - i - 1), true);
            i = skip_ws_and_comments(stmt, close_a + 1);
        }
    }

    // Parse optional semantic `: SEMANTIC` or initializer `= expr`
    std::string init_expr;
    if (i < stmt.size() && stmt[i] == ':') {
        while (i < stmt.size() && stmt[i] != '=' && stmt[i] != ';') ++i;
    }
    if (i < stmt.size() && stmt[i] == '=') {
        size_t eq_start = i + 1;
        size_t semi = stmt.rfind(';');
        if (semi != std::string::npos && semi > eq_start) {
            init_expr = trim_copy(stmt.substr(eq_start, semi - eq_start));
        }
    }

    // Emit valid HLSL uniform declaration without `< ... >` annotations
    out_hlsl << "uniform " << var_type << " " << var_name << ";\n";

    ParsedUniformInfo meta;
    meta.hlsl_type = var_type;
    meta.name = var_name;

    if (ann.count("source")) {
        meta.system_semantic = detect_system_semantic(ann["source"]);
        out_meta.push_back(meta);
        return;
    }

    if (ann.count("hidden") && to_lower_copy(ann["hidden"]) == "true") {
        out_meta.push_back(meta);
        return;
    }

    auto get_ann_str = [&](const char *k1, const char *k2 = nullptr) -> std::string {
        if (ann.count(k1)) return ann[k1];
        if (k2 && ann.count(k2)) return ann[k2];
        return {};
    };
    auto get_ann_float = [&](const char *k1, const char *k2, float def_val) -> float {
        std::string v = get_ann_str(k1, k2);
        if (v.empty()) return def_val;
        char *ep = nullptr;
        float r = std::strtof(v.c_str(), &ep);
        return (ep != v.c_str()) ? r : def_val;
    };

    gui::ShaderLabUniform u;
    u.name = var_name;
    u.label = get_ann_str("ui_label", "uiname");
    if (u.label.empty()) u.label = var_name;
    u.tooltip = get_ann_str("ui_tooltip", "uihelp");
    u.category = get_ann_str("ui_category");

    std::string ui_type = to_lower_copy(get_ann_str("ui_type", "uiwidget"));
    std::string tl = to_lower_copy(var_type);

    float parsed_vals[4] = {};
    int parsed_n = 0;
    parse_float_list(init_expr, parsed_vals, 4, parsed_n);

    if (tl == "bool") {
        u.type = gui::ShaderLabUniform::Type::Bool;
        bool def_b = (parsed_n > 0 && std::abs(parsed_vals[0]) > 0.5f);
        u.bool_val = def_b;
        u.default_bool = def_b;
    } else if (tl == "int" || tl == "uint") {
        std::string items = get_ann_str("ui_items");
        if (!items.empty() || ui_type == "combo" || ui_type == "radio") {
            u.type = gui::ShaderLabUniform::Type::Enum;
            size_t pos = 0;
            while (pos < items.size()) {
                size_t nul = items.find('\0', pos);
                std::string tok = (nul == std::string::npos) ? items.substr(pos) : items.substr(pos, nul - pos);
                if (!tok.empty()) u.enum_items.push_back(tok);
                if (nul == std::string::npos) break;
                pos = nul + 1;
            }
            if (u.enum_items.empty()) {
                u.enum_items = { "Option 0", "Option 1" };
            }
            int def_e = (parsed_n > 0) ? static_cast<int>(parsed_vals[0]) : 0;
            def_e = std::clamp(def_e, 0, static_cast<int>(u.enum_items.size()) - 1);
            u.enum_selected = def_e;
            u.default_enum = def_e;
        } else {
            u.type = gui::ShaderLabUniform::Type::Int;
            u.min_val = get_ann_float("ui_min", "uimin", 0.0f);
            u.max_val = get_ann_float("ui_max", "uimax", 10.0f);
            u.step = get_ann_float("ui_step", "uistep", 1.0f);
            int def_i = (parsed_n > 0) ? static_cast<int>(parsed_vals[0]) : static_cast<int>(u.min_val);
            u.int_val = def_i;
            u.default_int = def_i;
        }
    } else if (tl == "float3" || tl == "half3") {
        u.type = gui::ShaderLabUniform::Type::Color3;
        for (int k = 0; k < 3; ++k) {
            float v = (k < parsed_n) ? parsed_vals[k] : 1.0f;
            u.color_val[k] = v;
            u.default_color[k] = v;
        }
        u.color_val[3] = u.default_color[3] = 1.0f;
        u.min_val = get_ann_float("ui_min", "uimin", 0.0f);
        u.max_val = get_ann_float("ui_max", "uimax", 1.0f);
    } else if (tl == "float4" || tl == "half4" || tl == "float2" || tl == "half2") {
        u.type = (tl.back() == '2') ? gui::ShaderLabUniform::Type::Float : gui::ShaderLabUniform::Type::Color4;
        if (u.type == gui::ShaderLabUniform::Type::Float) {
            u.min_val = get_ann_float("ui_min", "uimin", 0.0f);
            u.max_val = get_ann_float("ui_max", "uimax", 2.0f);
            u.step = get_ann_float("ui_step", "uistep", 0.01f);
            float def_f = (parsed_n > 0) ? parsed_vals[0] : 0.5f;
            u.float_val = def_f;
            u.default_float = def_f;
        } else {
            for (int k = 0; k < 4; ++k) {
                float v = (k < parsed_n) ? parsed_vals[k] : 1.0f;
                u.color_val[k] = v;
                u.default_color[k] = v;
            }
            u.min_val = get_ann_float("ui_min", "uimin", 0.0f);
            u.max_val = get_ann_float("ui_max", "uimax", 1.0f);
        }
    } else {
        u.type = gui::ShaderLabUniform::Type::Float;
        u.min_val = get_ann_float("ui_min", "uimin", 0.0f);
        u.max_val = get_ann_float("ui_max", "uimax", 1.0f);
        u.step = get_ann_float("ui_step", "uistep", 0.01f);
        float def_f = (parsed_n > 0) ? parsed_vals[0] : u.min_val;
        if (u.max_val <= u.min_val) u.max_val = std::max(1.0f, def_f * 2.0f + 1.0f);
        u.float_val = def_f;
        u.default_float = def_f;
    }

    meta.ui_uniform_index = static_cast<int>(out_effect.uniforms.size());
    out_effect.uniforms.push_back(std::move(u));
    out_meta.push_back(std::move(meta));
}

size_t find_decl_semicolon(const std::string &s, size_t pos)
{
    bool in_str = false;
    bool seen_eq = false;
    int paren = 0;
    int brace = 0;
    size_t i = pos;
    while (i < s.size()) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) {
                i += 2;
                continue;
            }
            if (c == '"') in_str = false;
            ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            ++i;
            continue;
        }
        if (c == '<' && !seen_eq && paren == 0 && brace == 0) {
            size_t gt = find_matching_angle(s, i);
            if (gt != std::string::npos) {
                i = gt + 1;
                continue;
            }
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '=' && paren == 0 && brace == 0) seen_eq = true;
        else if (c == ';' && paren == 0 && brace == 0) {
            return i + 1;
        }
        ++i;
    }
    return i;
}

bool parse_reshade_or_hlsl_program(
    const std::string &preprocessed,
    ShaderDialect dialect,
    uint32_t buf_w,
    uint32_t buf_h,
    gui::ShaderLabEffect &out_effect,
    ParsedShaderProgram &out_prog,
    std::vector<std::string> &warnings)
{
    std::string flat = flatten_namespaces(preprocessed);
    std::ostringstream hlsl;
    std::unordered_map<std::string, std::string> sampler_to_tex;

    size_t i = 0;
    int brace_depth = 0;
    while (i < flat.size()) {
        if (std::isspace(static_cast<unsigned char>(flat[i]))) {
            hlsl << flat[i++];
            continue;
        }
        if (flat[i] == '/' && i + 1 < flat.size() && (flat[i + 1] == '/' || flat[i + 1] == '*')) {
            i = skip_ws_and_comments(flat, i);
            hlsl << ' ';
            continue;
        }

        if (brace_depth == 0 && starts_with_word(flat, i, "uniform")) {
            size_t semi = find_decl_semicolon(flat, i);
            parse_uniform_declaration(flat.substr(i, semi - i), out_effect, out_prog.uniforms_meta, hlsl);
            i = semi;
            continue;
        }

        if (starts_with_word(flat, i, "texture") || starts_with_word(flat, i, "texture2D")) {
            bool is2d = starts_with_word(flat, i, "texture2D");
            size_t j = skip_ws_and_comments(flat, i + (is2d ? 9 : 7));
            size_t n_st = j;
            while (j < flat.size() && is_ident_char(flat[j])) ++j;
            std::string tname = flat.substr(n_st, j - n_st);
            j = skip_ws_and_comments(flat, j);

            if (j < flat.size() && flat[j] == '<') {
                size_t ca = find_matching_angle(flat, j);
                if (ca != std::string::npos) j = skip_ws_and_comments(flat, ca + 1);
            }

            ParsedTextureDecl td;
            td.name = tname;
            if (j < flat.size() && flat[j] == ':') {
                ++j;
                j = skip_ws_and_comments(flat, j);
                size_t s_st = j;
                while (j < flat.size() && is_ident_char(flat[j])) ++j;
                td.semantic = to_lower_copy(flat.substr(s_st, j - s_st));
                j = skip_ws_and_comments(flat, j);
            }
            if (j < flat.size() && flat[j] == '{') {
                size_t cb = find_matching_delim(flat, j, '{', '}');
                if (cb != std::string::npos) {
                    auto kv = parse_key_value_block(flat.substr(j + 1, cb - j - 1), false);
                    if (kv.count("width")) {
                        std::string w_expr = kv["width"];
                        double w_val = eval_dim_expr(w_expr, buf_w, buf_h);
                        if (w_expr.find("BUFFER_WIDTH") != std::string::npos) {
                            td.rt_spec.width_scale = static_cast<float>(w_val / std::max(1u, buf_w));
                        } else if (w_val >= 1.0) {
                            td.rt_spec.explicit_width = static_cast<uint32_t>(std::llround(w_val));
                        }
                    }
                    if (kv.count("height")) {
                        std::string h_expr = kv["height"];
                        double h_val = eval_dim_expr(h_expr, buf_w, buf_h);
                        if (h_expr.find("BUFFER_HEIGHT") != std::string::npos) {
                            td.rt_spec.height_scale = static_cast<float>(h_val / std::max(1u, buf_h));
                        } else if (h_val >= 1.0) {
                            td.rt_spec.explicit_height = static_cast<uint32_t>(std::llround(h_val));
                        }
                    }
                    if (kv.count("format")) td.rt_spec.format = parse_reshade_format(kv["format"]);
                    if (kv.count("miplevels")) td.rt_spec.mip_levels = std::max(1, std::atoi(kv["miplevels"].c_str()));
                    j = skip_ws_and_comments(flat, cb + 1);
                }
            }
            if (j < flat.size() && flat[j] == ';') ++j;

            td.is_backbuffer = (td.semantic == "color" || td.name == "ReShade_BackBufferTex" || td.name == "BackBufferTex");
            td.is_depth = (td.semantic == "depth" || td.name == "ReShade_DepthBufferTex" || td.name == "DepthBufferTex");
            td.rt_spec.name = tname;
            if (!tname.empty()) {
                out_prog.textures.push_back(std::move(td));
            }
            i = j;
            continue;
        }

        if (starts_with_word(flat, i, "sampler") || starts_with_word(flat, i, "sampler2D")) {
            bool is2d = starts_with_word(flat, i, "sampler2D");
            size_t j = skip_ws_and_comments(flat, i + (is2d ? 9 : 7));
            size_t n_st = j;
            while (j < flat.size() && is_ident_char(flat[j])) ++j;
            std::string sname = flat.substr(n_st, j - n_st);
            j = skip_ws_and_comments(flat, j);
            if (j < flat.size() && flat[j] == '<') {
                size_t ca = find_matching_angle(flat, j);
                if (ca != std::string::npos) j = skip_ws_and_comments(flat, ca + 1);
            }
            // Only parse top-level sampler declarations followed by '{' or ';'
            if (j < flat.size() && (flat[j] == '{' || flat[j] == ';')) {
                ParsedSamplerDecl sd;
                sd.name = sname;
                if (flat[j] == '{') {
                    size_t cb = find_matching_delim(flat, j, '{', '}');
                    if (cb != std::string::npos) {
                        auto kv = parse_key_value_block(flat.substr(j + 1, cb - j - 1), false);
                        if (kv.count("texture")) sd.tex_name = trim_copy(kv["texture"]);
                        if (kv.count("magfilter")) {
                            std::string f = to_lower_copy(kv["magfilter"]);
                            if (f == "point") sd.filter_mode = 1;
                        }
                        auto parse_addr = [](const std::string &a) {
                            std::string al = to_lower_copy(a);
                            if (al == "wrap" || al == "repeat") return 1;
                            if (al == "mirror") return 2;
                            if (al == "border") return 3;
                            return 0;
                        };
                        if (kv.count("addressu")) sd.address_u = parse_addr(kv["addressu"]);
                        if (kv.count("addressv")) sd.address_v = parse_addr(kv["addressv"]);
                        j = skip_ws_and_comments(flat, cb + 1);
                    }
                }
                if (j < flat.size() && flat[j] == ';') ++j;
                if (!sname.empty()) {
                    if (sd.tex_name.empty()) sd.tex_name = "BackBufferTex";
                    sampler_to_tex[sname] = sd.tex_name;
                    out_prog.samplers.push_back(std::move(sd));
                }
                i = j;
                continue;
            }
        }

        if (starts_with_word(flat, i, "technique")) {
            size_t j = skip_ws_and_comments(flat, i + 9);
            while (j < flat.size() && is_ident_char(flat[j])) ++j;
            j = skip_ws_and_comments(flat, j);
            if (j < flat.size() && flat[j] == '<') {
                size_t ca = find_matching_angle(flat, j);
                if (ca != std::string::npos) j = skip_ws_and_comments(flat, ca + 1);
            }
            if (j < flat.size() && flat[j] == '{') {
                size_t cb = find_matching_delim(flat, j, '{', '}');
                if (cb != std::string::npos) {
                    // Only collect passes from the first enabled technique
                    if (out_prog.passes.empty()) {
                        std::string tech_body = flat.substr(j + 1, cb - j - 1);
                        size_t p = 0;
                        while (p < tech_body.size()) {
                            p = skip_ws_and_comments(tech_body, p);
                            if (p >= tech_body.size()) break;
                            if (starts_with_word(tech_body, p, "pass")) {
                                size_t q = skip_ws_and_comments(tech_body, p + 4);
                                size_t pn_s = q;
                                while (q < tech_body.size() && is_ident_char(tech_body[q])) ++q;
                                ParsedPassDecl pass;
                                pass.pass_name = tech_body.substr(pn_s, q - pn_s);
                                q = skip_ws_and_comments(tech_body, q);
                                if (q < tech_body.size() && tech_body[q] == '{') {
                                    size_t pb = find_matching_delim(tech_body, q, '{', '}');
                                    if (pb != std::string::npos) {
                                        auto kv = parse_key_value_block(tech_body.substr(q + 1, pb - q - 1), false);
                                        if (kv.count("vertexshader")) pass.vs_entry = kv["vertexshader"];
                                        if (kv.count("pixelshader")) pass.ps_entry = kv["pixelshader"];
                                        if (kv.count("rendertarget")) pass.render_targets.push_back(kv["rendertarget"]);
                                        for (int rt = 0; rt < 8; ++rt) {
                                            std::string rk = "rendertarget" + std::to_string(rt);
                                            if (kv.count(rk)) pass.render_targets.push_back(kv[rk]);
                                        }
                                        if (kv.count("clearrendertargets")) {
                                            pass.clear_rt = (to_lower_copy(kv["clearrendertargets"]) == "true");
                                        }
                                        if (kv.count("srgbwriteenable")) {
                                            pass.srgb_write = (to_lower_copy(kv["srgbwriteenable"]) == "true");
                                        }
                                        if (!pass.ps_entry.empty()) {
                                            out_prog.passes.push_back(std::move(pass));
                                        }
                                        p = pb + 1;
                                        continue;
                                    }
                                }
                            }
                            ++p;
                        }
                    }
                    i = cb + 1;
                    if (i < flat.size() && flat[i] == ';') ++i;
                    continue;
                }
            }
        }

        if (dialect == ShaderDialect::RawHlsl && starts_with_word(flat, i, "cbuffer")) {
            size_t ob = flat.find('{', i);
            size_t cb = (ob != std::string::npos) ? find_matching_delim(flat, ob, '{', '}') : std::string::npos;
            if (cb != std::string::npos) {
                hlsl << flat.substr(i, ob - i + 1) << "\n";
                std::string body = flat.substr(ob + 1, cb - ob - 1);
                size_t p = 0;
                while (p < body.size()) {
                    p = skip_ws_and_comments(body, p);
                    if (p >= body.size()) break;
                    size_t semi = find_decl_semicolon(body, p);
                    std::string decl = trim_copy(body.substr(p, semi - p));
                    if (!decl.empty()) {
                        std::ostringstream dummy;
                        std::string fake_u = decl;
                        if (fake_u.rfind("uniform ", 0) != 0) fake_u = "uniform " + fake_u;
                        parse_uniform_declaration(fake_u, out_effect, out_prog.uniforms_meta, dummy);
                        std::string cleaned_member = dummy.str();
                        if (cleaned_member.rfind("uniform ", 0) == 0) cleaned_member.erase(0, 8);
                        hlsl << "    " << (cleaned_member.empty() ? decl : cleaned_member);
                    }
                    p = semi;
                }
                i = cb + 1;
                hlsl << "}";
                if (i < flat.size() && flat[i] == ';') { hlsl << ";"; ++i; }
                hlsl << "\n";
                continue;
            }
        }

        if (dialect == ShaderDialect::RawHlsl &&
            (starts_with_word(flat, i, "float") || starts_with_word(flat, i, "float2") ||
             starts_with_word(flat, i, "float3") || starts_with_word(flat, i, "float4") ||
             starts_with_word(flat, i, "int") || starts_with_word(flat, i, "bool"))) {
            // Check for top-level DXSAS variable declaration `float name < UIName = ...; > = 0.5;`
            size_t j = i;
            while (j < flat.size() && is_ident_char(flat[j])) ++j;
            j = skip_ws_and_comments(flat, j);
            size_t n_s = j;
            while (j < flat.size() && is_ident_char(flat[j])) ++j;
            size_t after_n = skip_ws_and_comments(flat, j);
            if (j > n_s && after_n < flat.size() && flat[after_n] == '<') {
                size_t semi = find_decl_semicolon(flat, i);
                std::string fake_u = "uniform " + flat.substr(i, semi - i);
                parse_uniform_declaration(fake_u, out_effect, out_prog.uniforms_meta, hlsl);
                i = semi;
                continue;
            }
        }

        // Copy raw top-level statement or function block verbatim
        if (flat[i] == '"') {
            hlsl << flat[i++];
            while (i < flat.size()) {
                char c = flat[i++];
                hlsl << c;
                if (c == '\\' && i < flat.size()) hlsl << flat[i++];
                else if (c == '"') break;
            }
        } else if (is_ident_start(flat[i])) {
            while (i < flat.size() && is_ident_char(flat[i])) {
                hlsl << flat[i++];
            }
        } else {
            char c = flat[i++];
            if (c == '{') ++brace_depth;
            else if (c == '}' && brace_depth > 0) --brace_depth;
            hlsl << c;
        }
    }

    std::string raw_hlsl = hlsl.str();
    raw_hlsl = rewrite_sampler_function_params(raw_hlsl);
    raw_hlsl = rewrite_tex2d_offset_and_gather(raw_hlsl, sampler_to_tex);

    if (dialect == ShaderDialect::ReShadeFx) {
        out_prog.hlsl_body = build_reshade_preamble(out_prog, buf_w, buf_h) + "\n" + raw_hlsl;
    } else {
        // Raw HLSL: inject fallback viewport/time globals and fullscreen VS if missing
        std::ostringstream prefix;
        prefix << "float4 __sl_system_viewport;\n";
        prefix << "float4 __sl_system_time;\n";
        prefix << "int __sl_framecount_i;\n";
        out_prog.hlsl_body = prefix.str() + "\n" + raw_hlsl;
    }

    if (out_prog.passes.empty()) {
        // Auto-discover VS and PS entries for raw HLSL files without `technique`
        auto has_fn = [&](const std::string &fn) -> bool {
            size_t pos = 0;
            while ((pos = out_prog.hlsl_body.find(fn, pos)) != std::string::npos) {
                bool left_ok = (pos == 0 || !is_ident_char(out_prog.hlsl_body[pos - 1]));
                size_t after = pos + fn.size();
                if (left_ok && after < out_prog.hlsl_body.size()) {
                    size_t ws = skip_ws_and_comments(out_prog.hlsl_body, after);
                    if (ws < out_prog.hlsl_body.size() && out_prog.hlsl_body[ws] == '(') {
                        return true;
                    }
                }
                pos = after;
            }
            return false;
        };

        std::string vs_entry;
        for (const char *cand : { "VSMain", "VS", "mainVS", "vs_main", "PostProcessVS", "vert" }) {
            if (has_fn(cand)) { vs_entry = cand; break; }
        }
        if (vs_entry.empty()) {
            out_prog.hlsl_body += R"HLSL(
void __ShaderLabFullscreenVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";
            vs_entry = "__ShaderLabFullscreenVS";
        }

        std::string ps_entry;
        for (const char *cand : { "PSMain", "PS", "mainPS", "ps_main", "PixelShader", "frag", "main" }) {
            if (has_fn(cand)) { ps_entry = cand; break; }
        }
        if (ps_entry.empty()) {
            // Look for function ending with `) : SV_Target` or `) : COLOR`
            size_t svt = out_prog.hlsl_body.find("SV_Target");
            if (svt == std::string::npos) svt = out_prog.hlsl_body.find(": COLOR");
            if (svt != std::string::npos) {
                size_t rp = out_prog.hlsl_body.rfind(')', svt);
                if (rp != std::string::npos) {
                    int depth = 0;
                    size_t q = rp;
                    while (q > 0) {
                        if (out_prog.hlsl_body[q] == ')') ++depth;
                        else if (out_prog.hlsl_body[q] == '(') {
                            --depth;
                            if (depth == 0) break;
                        }
                        --q;
                    }
                    if (q > 0) {
                        size_t end_id = q;
                        while (end_id > 0 && std::isspace(static_cast<unsigned char>(out_prog.hlsl_body[end_id - 1]))) --end_id;
                        size_t st_id = end_id;
                        while (st_id > 0 && is_ident_char(out_prog.hlsl_body[st_id - 1])) --st_id;
                        if (end_id > st_id) ps_entry = out_prog.hlsl_body.substr(st_id, end_id - st_id);
                    }
                }
            }
        }

        if (ps_entry.empty()) {
            warnings.push_back("Could not identify pixel shader entry point; defaulting to PSMain.");
            ps_entry = "PSMain";
        }

        ParsedPassDecl pass;
        pass.pass_name = "MainPass";
        pass.vs_entry = vs_entry;
        pass.ps_entry = ps_entry;
        out_prog.passes.push_back(std::move(pass));
    }

    return true;
}

// -------------------- Unity .shader Parser --------------------

bool parse_unity_shader_program(
    const std::string &source,
    uint32_t buf_w,
    uint32_t buf_h,
    gui::ShaderLabEffect &out_effect,
    ParsedShaderProgram &out_prog,
    std::vector<std::string> &warnings)
{
    std::string cleaned = strip_comments_preserve_strings(source);

    // 1) Parse Properties { ... }
    std::vector<std::string> scalar_decls;
    size_t prop_pos = cleaned.find("Properties");
    if (prop_pos != std::string::npos) {
        size_t ob = cleaned.find('{', prop_pos);
        size_t cb = (ob != std::string::npos) ? find_matching_delim(cleaned, ob, '{', '}') : std::string::npos;
        if (cb != std::string::npos) {
            std::string body = cleaned.substr(ob + 1, cb - ob - 1);
            std::stringstream bss(body);
            std::string line;
            while (std::getline(bss, line)) {
                std::string t = trim_copy(line);
                if (t.empty()) continue;
                bool is_toggle = (t.find("[Toggle") != std::string::npos);
                // Strip attribute brackets [...] at start of line
                while (!t.empty() && t.front() == '[') {
                    size_t rb = t.find(']');
                    if (rb == std::string::npos) break;
                    t = trim_copy(t.substr(rb + 1));
                }
                size_t lp = t.find('(');
                if (lp == std::string::npos) continue;
                std::string pname = trim_copy(t.substr(0, lp));
                size_t rp = find_matching_delim(t, lp, '(', ')');
                if (rp == std::string::npos) continue;

                std::string inside = t.substr(lp + 1, rp - lp - 1);
                size_t q1 = inside.find('"');
                size_t q2 = (q1 != std::string::npos) ? inside.find('"', q1 + 1) : std::string::npos;
                std::string plabel = (q2 != std::string::npos) ? inside.substr(q1 + 1, q2 - q1 - 1) : pname;
                size_t comma = (q2 != std::string::npos) ? inside.find(',', q2 + 1) : inside.find(',');
                std::string ptype = (comma != std::string::npos) ? trim_copy(inside.substr(comma + 1)) : "Float";

                size_t eq = t.find('=', rp + 1);
                std::string def_expr = (eq != std::string::npos) ? trim_copy(t.substr(eq + 1)) : "0";
                std::string ptype_low = to_lower_copy(ptype);

                if (ptype_low.find("2d") != std::string::npos || ptype_low.find("cube") != std::string::npos || ptype_low.find("3d") != std::string::npos) {
                    continue;
                }

                gui::ShaderLabUniform u;
                u.name = pname;
                u.label = plabel.empty() ? pname : plabel;
                float vals[4] = {};
                int nvals = 0;
                parse_float_list(def_expr, vals, 4, nvals);

                std::string hlsl_t = "float";
                if (ptype_low.rfind("range", 0) == 0) {
                    u.type = gui::ShaderLabUniform::Type::Float;
                    size_t rlp = ptype.find('(');
                    size_t rrp = ptype.find(')');
                    if (rlp != std::string::npos && rrp != std::string::npos) {
                        float rvals[2] = { 0.0f, 1.0f };
                        int rn = 0;
                        parse_float_list(ptype.substr(rlp, rrp - rlp + 1), rvals, 2, rn);
                        if (rn >= 2) { u.min_val = rvals[0]; u.max_val = rvals[1]; }
                    }
                    u.step = (u.max_val - u.min_val) / 100.0f;
                    u.float_val = u.default_float = (nvals > 0) ? vals[0] : u.min_val;
                } else if (ptype_low == "int") {
                    u.type = gui::ShaderLabUniform::Type::Int;
                    u.min_val = 0.0f;
                    u.max_val = 32.0f;
                    u.step = 1.0f;
                    u.int_val = u.default_int = (nvals > 0) ? static_cast<int>(vals[0]) : 0;
                    hlsl_t = "int";
                } else if (ptype_low == "color" || ptype_low == "vector") {
                    u.type = gui::ShaderLabUniform::Type::Color4;
                    for (int k = 0; k < 4; ++k) {
                        u.color_val[k] = u.default_color[k] = (k < nvals) ? vals[k] : 1.0f;
                    }
                    hlsl_t = "float4";
                } else if (is_toggle) {
                    u.type = gui::ShaderLabUniform::Type::Bool;
                    u.bool_val = u.default_bool = (nvals > 0 && vals[0] > 0.5f);
                } else {
                    u.type = gui::ShaderLabUniform::Type::Float;
                    u.min_val = 0.0f;
                    u.max_val = std::max(2.0f, (nvals > 0 ? std::abs(vals[0]) * 4.0f : 2.0f));
                    u.step = 0.01f;
                    u.float_val = u.default_float = (nvals > 0) ? vals[0] : 0.0f;
                }

                ParsedUniformInfo meta;
                meta.name = pname;
                meta.hlsl_type = hlsl_t;
                meta.ui_uniform_index = static_cast<int>(out_effect.uniforms.size());
                out_effect.uniforms.push_back(std::move(u));
                out_prog.uniforms_meta.push_back(std::move(meta));
                scalar_decls.push_back(pname);
            }
        }
    }

    // 2) Extract first CGPROGRAM..ENDCG or HLSLPROGRAM..ENDHLSL block
    size_t blk_start = cleaned.find("CGPROGRAM");
    size_t blk_end = std::string::npos;
    size_t hdr_len = 9;
    if (blk_start != std::string::npos) {
        blk_end = cleaned.find("ENDCG", blk_start + 9);
    } else {
        blk_start = cleaned.find("HLSLPROGRAM");
        hdr_len = 11;
        if (blk_start != std::string::npos) blk_end = cleaned.find("ENDHLSL", blk_start + 11);
    }
    if (blk_start == std::string::npos || blk_end == std::string::npos) {
        warnings.push_back("Unity .shader missing CGPROGRAM/HLSLPROGRAM block.");
        return false;
    }

    std::string raw_cg = cleaned.substr(blk_start + hdr_len, blk_end - (blk_start + hdr_len));
    std::string vs_entry = "vert";
    std::string ps_entry = "frag";

    std::stringstream rss(raw_cg);
    std::ostringstream filtered_cg;
    std::string line;
    while (std::getline(rss, line)) {
        std::string t = trim_copy(line);
        if (t.rfind("#pragma vertex ", 0) == 0) {
            vs_entry = trim_copy(t.substr(15));
            continue;
        }
        if (t.rfind("#pragma fragment ", 0) == 0) {
            ps_entry = trim_copy(t.substr(17));
            continue;
        }
        if (t.rfind("#include", 0) == 0 &&
            (t.find("UnityCG") != std::string::npos || t.find("Core.hlsl") != std::string::npos ||
             t.find("Lighting") != std::string::npos)) {
            continue;
        }
        filtered_cg << line << "\n";
    }

    // Unity shim providing fixed types, _Time, _ScreenParams, appdata, v2f, UnityObjectToClipPos, tex2D
    std::ostringstream final_hlsl;
    final_hlsl << R"HLSL(
#define fixed float
#define fixed2 float2
#define fixed3 float3
#define fixed4 float4
#define UNITY_PI 3.14159265359
#define UNITY_INITIALIZE_OUTPUT(t, v) (v = (t)0)
#define TRANSFORM_TEX(tex, name) (tex)
#define UnityObjectToClipPos(pos) (float4((pos).xyz, 1.0))
#define SV_POSITION SV_Position
#define COLOR SV_Target

float4 _Time;
float4 _SinTime;
float4 _ScreenParams;
float4 _MainTex_TexelSize;
Texture2D _MainTex;
Texture2D _CameraDepthTexture;
SamplerState sampler_MainTex;
SamplerState sampler_CameraDepthTexture;

float4 tex2D(sampler2D_dummy) { return 0; }
#define UNITY_DECLARE_TEX2D(tex) Texture2D tex; SamplerState sampler##tex
#define UNITY_SAMPLE_TEX2D(tex, coord) tex.Sample(sampler##tex, coord)
#define TEXTURE2D(tex) Texture2D tex
#define SAMPLER(smp) SamplerState smp
#define SAMPLE_TEXTURE2D(tex, smp, coord) ((tex).Sample((smp), (coord)))
#define real float
#define real2 float2
#define real3 float3
#define real4 float4

float Luminance(float3 rgb) { return dot(rgb, float3(0.2126, 0.7152, 0.0722)); }
)HLSL";

    // Handle `sampler2D` and duplicate built-in Texture2D/SamplerState declarations cleanly
    std::string cg_body = filtered_cg.str();
    std::string rewritten_cg;
    {
        auto is_builtin_tex_or_smp = [](const std::string &sym) -> bool {
            return sym == "_MainTex" || sym == "_CameraDepthTexture" ||
                   sym == "sampler_MainTex" || sym == "sampler_CameraDepthTexture";
        };
        auto extract_macro_arg = [](const std::string &stmt, const char *macro_name) -> std::string {
            std::string trimmed = trim_copy(stmt);
            size_t mlen = std::strlen(macro_name);
            if (trimmed.rfind(macro_name, 0) != 0) return {};
            size_t p = mlen;
            while (p < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[p]))) ++p;
            if (p >= trimmed.size() || trimmed[p] != '(') return {};
            size_t rp = trimmed.find(')', p + 1);
            if (rp == std::string::npos) return {};
            return trim_copy(trimmed.substr(p + 1, rp - p - 1));
        };

        std::stringstream bss(cg_body);
        std::string ln;
        while (std::getline(bss, ln)) {
            std::string tr = trim_copy(ln);
            // Split semicolon-separated top-level declarations on the same line if no `{`/`(`
            if (tr.find('{') == std::string::npos && tr.find(';') != std::string::npos) {
                std::string rebuilt_ln;
                size_t pos = 0;
                bool handled_any = false;
                while (pos < tr.size()) {
                    size_t semi = tr.find(';', pos);
                    if (semi == std::string::npos) {
                        rebuilt_ln += tr.substr(pos);
                        break;
                    }
                    std::string stmt = trim_copy(tr.substr(pos, semi - pos));
                    pos = semi + 1;
                    if (stmt.empty()) continue;

                    std::string macro_arg = extract_macro_arg(stmt, "TEXTURE2D");
                    if (macro_arg.empty()) macro_arg = extract_macro_arg(stmt, "SAMPLER");
                    if (macro_arg.empty()) macro_arg = extract_macro_arg(stmt, "UNITY_DECLARE_TEX2D");
                    if (!macro_arg.empty()) {
                        if (is_builtin_tex_or_smp(macro_arg)) {
                            handled_any = true;
                            continue;
                        }
                    }

                    // Parse `<type> <ident>` (supports sampler2D, sampler2D_float, Texture2D, Texture2D<float4>, SamplerState)
                    size_t sp = 0;
                    while (sp < stmt.size() && is_ident_char(stmt[sp])) ++sp;
                    std::string kw = stmt.substr(0, sp);
                    size_t after_kw = sp;
                    while (after_kw < stmt.size() && std::isspace(static_cast<unsigned char>(stmt[after_kw]))) ++after_kw;
                    if (kw == "Texture2D" && after_kw < stmt.size() && stmt[after_kw] == '<') {
                        size_t ca = stmt.find('>', after_kw);
                        if (ca != std::string::npos) {
                            after_kw = ca + 1;
                            while (after_kw < stmt.size() && std::isspace(static_cast<unsigned char>(stmt[after_kw]))) ++after_kw;
                        }
                    }
                    std::string rest_sym = (after_kw < stmt.size()) ? trim_copy(stmt.substr(after_kw)) : std::string();
                    if ((kw == "sampler2D" || kw == "sampler2D_float") && stmt.find('(') == std::string::npos && !rest_sym.empty()) {
                        handled_any = true;
                        if (!is_builtin_tex_or_smp(rest_sym)) {
                            rebuilt_ln += "Texture2D " + rest_sym + "; SamplerState sampler" + rest_sym + "; ";
                        }
                        continue;
                    }
                    if ((kw == "Texture2D" || kw == "SamplerState") && stmt.find('(') == std::string::npos &&
                        is_builtin_tex_or_smp(rest_sym)) {
                        handled_any = true;
                        continue;
                    }
                    rebuilt_ln += stmt + "; ";
                }
                if (handled_any) {
                    if (!rebuilt_ln.empty()) rewritten_cg += rebuilt_ln + "\n";
                    continue;
                }
            }
            rewritten_cg += ln + "\n";
        }
    }

    // Conditionally emit fallback Unity structs only when not declared in shader source
    auto has_struct_def = [](const std::string &code, const std::string &sname) -> bool {
        size_t pos = 0;
        while ((pos = code.find("struct", pos)) != std::string::npos) {
            bool left_ok = (pos == 0 || !is_ident_char(code[pos - 1]));
            size_t after = pos + 6;
            if (left_ok && after < code.size() && std::isspace(static_cast<unsigned char>(code[after]))) {
                size_t ns = skip_ws_and_comments(code, after);
                size_t ne = ns;
                while (ne < code.size() && is_ident_char(code[ne])) ++ne;
                if (code.substr(ns, ne - ns) == sname) {
                    size_t brace = skip_ws_and_comments(code, ne);
                    if (brace < code.size() && code[brace] == '{') {
                        return true;
                    }
                }
            }
            pos = after;
        }
        return false;
    };
    if (!has_struct_def(rewritten_cg, "appdata")) {
        final_hlsl << "struct appdata {\n    float4 vertex : POSITION;\n    float2 uv : TEXCOORD0;\n};\n";
    }
    if (!has_struct_def(rewritten_cg, "appdata_img")) {
        final_hlsl << "struct appdata_img {\n    float4 vertex : POSITION;\n    float2 texcoord : TEXCOORD0;\n};\n";
    }
    if (!has_struct_def(rewritten_cg, "v2f")) {
        final_hlsl << "struct v2f {\n    float4 pos : SV_Position;\n    float2 uv : TEXCOORD0;\n};\n";
    }

    // Rewrite tex2D(_MainTex, uv) -> _MainTex.Sample(sampler_MainTex, uv)
    {
        std::string out_t;
        size_t i = 0;
        while (i < rewritten_cg.size()) {
            if (starts_with_word(rewritten_cg, i, "tex2D")) {
                size_t lp = skip_ws_and_comments(rewritten_cg, i + 5);
                if (lp < rewritten_cg.size() && rewritten_cg[lp] == '(') {
                    size_t rp = find_matching_delim(rewritten_cg, lp, '(', ')');
                    if (rp != std::string::npos) {
                        auto args = split_top_level_args(rewritten_cg.substr(lp + 1, rp - lp - 1));
                        if (args.size() >= 2) {
                            std::string tname = trim_copy(args[0]);
                            out_t += "(" + tname + ".Sample(sampler" + tname + ", (" + args[1] + ")))";
                            i = rp + 1;
                            continue;
                        }
                    }
                }
            }
            out_t.push_back(rewritten_cg[i++]);
        }
        rewritten_cg = std::move(out_t);
    }

    // Remove dummy overload `tex2D(sampler2D_dummy)` cleanly and replace with user CG code
    std::string final_str = final_hlsl.str();
    size_t dum = final_str.find("float4 tex2D(sampler2D_dummy) { return 0; }");
    if (dum != std::string::npos) final_str.erase(dum, 43);

    // Strip duplicate declarations of _MainTex_TexelSize if shader already wrote them
    auto strip_dup_decl = [&](const std::string &sym) {
        size_t p = rewritten_cg.find(sym);
        while (p != std::string::npos) {
            size_t ls = rewritten_cg.rfind('\n', p);
            ls = (ls == std::string::npos) ? 0 : ls + 1;
            size_t le = rewritten_cg.find('\n', p);
            if (le == std::string::npos) le = rewritten_cg.size();
            std::string ln = trim_copy(rewritten_cg.substr(ls, le - ls));
            if (ln.find(';') != std::string::npos && ln.find('(') == std::string::npos &&
                (ln.find("float4") == 0 || ln.find("half4") == 0 || ln.find("uniform") == 0)) {
                rewritten_cg.erase(ls, le - ls);
                p = rewritten_cg.find(sym, ls);
            } else {
                p = rewritten_cg.find(sym, p + sym.size());
            }
        }
    };
    strip_dup_decl("_MainTex_TexelSize");
    strip_dup_decl("_Time");
    strip_dup_decl("_ScreenParams");

    final_str += "\n" + rewritten_cg;

    // Wrap fullscreen SV_VertexID VS calling Unity `vert` or fallback
    final_str += R"HLSL(
void __ShaderLabUnityVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";

    // Register Unity system uniforms so EffectPipeline updates them automatically
    auto add_sys = [&](const char *n, SystemUniformSemantic sem) {
        ParsedUniformInfo mi;
        mi.name = n;
        mi.system_semantic = sem;
        out_prog.uniforms_meta.push_back(mi);
    };
    add_sys("_Time", SystemUniformSemantic::UnityTime);
    add_sys("_SinTime", SystemUniformSemantic::UnitySinTime);
    add_sys("_ScreenParams", SystemUniformSemantic::UnityScreenParams);
    add_sys("_MainTex_TexelSize", SystemUniformSemantic::TexelSize);

    out_prog.hlsl_body = final_str;
    ParsedPassDecl pass;
    pass.pass_name = "UnityPass";
    pass.vs_entry = "__ShaderLabUnityVS";
    pass.ps_entry = ps_entry;
    out_prog.passes.push_back(std::move(pass));
    (void)vs_entry;
    (void)buf_w;
    (void)buf_h;
    return true;
}

// -------------------- Godot .gdshader Transpiler --------------------

std::string expand_scalar_vec_ctors(const std::string &code) {
    // Expands single-arg float2(s) -> float2(s, s), float3(s) -> float3(s, s, s), float4(s) -> float4(s, s, s, s)
    std::string out;
    out.reserve(code.size());
    size_t i = 0;
    while (i < code.size()) {
        int dim = 0;
        const char *pfx = nullptr;
        if (starts_with_word(code, i, "float2")) { dim = 2; pfx = "float2"; }
        else if (starts_with_word(code, i, "float3")) { dim = 3; pfx = "float3"; }
        else if (starts_with_word(code, i, "float4")) { dim = 4; pfx = "float4"; }
        if (dim > 0) {
            size_t lp = skip_ws_and_comments(code, i + 6);
            if (lp < code.size() && code[lp] == '(') {
                size_t rp = find_matching_delim(code, lp, '(', ')');
                if (rp != std::string::npos) {
                    auto args = split_top_level_args(code.substr(lp + 1, rp - lp - 1));
                    if (args.size() == 1 && !args[0].empty()) {
                        out += "__sl_mkf" + std::to_string(dim) + "(" + args[0] + ")";
                        i = rp + 1;
                        continue;
                    }
                }
            }
        }
        out.push_back(code[i++]);
        (void)pfx;
    }
    return out;
}

bool parse_godot_gdshader_program(
    const std::string &source,
    uint32_t buf_w,
    uint32_t buf_h,
    gui::ShaderLabEffect &out_effect,
    ParsedShaderProgram &out_prog,
    std::vector<std::string> &warnings)
{
    std::string cleaned = strip_comments_preserve_strings(source);
    std::ostringstream body_ss;

    size_t i = 0;
    while (i < cleaned.size()) {
        if (std::isspace(static_cast<unsigned char>(cleaned[i]))) {
            body_ss << cleaned[i++];
            continue;
        }

        if (starts_with_word(cleaned, i, "shader_type") || starts_with_word(cleaned, i, "render_mode")) {
            size_t semi = cleaned.find(';', i);
            i = (semi != std::string::npos) ? semi + 1 : cleaned.size();
            continue;
        }

        if (starts_with_word(cleaned, i, "uniform")) {
            size_t semi = cleaned.find(';', i);
            if (semi == std::string::npos) semi = cleaned.size();
            std::string stmt = cleaned.substr(i, semi - i);
            i = (semi < cleaned.size()) ? semi + 1 : cleaned.size();

            // Example: uniform vec4 tint : source_color = vec4(1.0);
            // Example: uniform float amt : hint_range(0.0, 2.0, 0.05) = 0.5;
            size_t p = skip_ws_and_comments(stmt, 7);
            size_t ts = p;
            while (p < stmt.size() && is_ident_char(stmt[p])) ++p;
            std::string gtype = stmt.substr(ts, p - ts);
            p = skip_ws_and_comments(stmt, p);
            size_t ns = p;
            while (p < stmt.size() && is_ident_char(stmt[p])) ++p;
            std::string gname = stmt.substr(ns, p - ns);

            std::string tail = stmt.substr(p);
            std::string tail_low = to_lower_copy(tail);
            if (gtype == "sampler2D" || gtype == "samplerCube" || gtype == "sampler3D") {
                if (tail_low.find("hint_depth") != std::string::npos) {
                    body_ss << "#define " << gname << " __sl_godot_depth_tex\n";
                } else {
                    body_ss << "#define " << gname << " __sl_godot_tex\n";
                }
                continue;
            }

            std::string hlsl_t = "float";
            if (gtype == "vec2") hlsl_t = "float2";
            else if (gtype == "vec3") hlsl_t = "float3";
            else if (gtype == "vec4") hlsl_t = "float4";
            else if (gtype == "int") hlsl_t = "int";
            else if (gtype == "uint") hlsl_t = "uint";
            else if (gtype == "bool") hlsl_t = "bool";

            body_ss << "uniform " << hlsl_t << " " << gname << ";\n";

            gui::ShaderLabUniform u;
            u.name = gname;
            u.label = gname;

            float vals[4] = {};
            int nvals = 0;
            size_t eq_pos = tail.find('=');
            if (eq_pos != std::string::npos) {
                parse_float_list(tail.substr(eq_pos + 1), vals, 4, nvals);
            }

            float hmin = 0.0f, hmax = 1.0f, hstep = 0.01f;
            size_t hr = tail_low.find("hint_range");
            if (hr != std::string::npos) {
                size_t lp = tail.find('(', hr);
                size_t rp = (lp != std::string::npos) ? find_matching_delim(tail, lp, '(', ')') : std::string::npos;
                if (rp != std::string::npos) {
                    float hr_vals[3] = {};
                    int hr_n = 0;
                    parse_float_list(tail.substr(lp, rp - lp + 1), hr_vals, 3, hr_n);
                    if (hr_n >= 2) { hmin = hr_vals[0]; hmax = hr_vals[1]; }
                    if (hr_n >= 3) { hstep = hr_vals[2]; }
                }
            }

            if (gtype == "bool") {
                u.type = gui::ShaderLabUniform::Type::Bool;
                u.bool_val = u.default_bool = (nvals > 0 && vals[0] > 0.5f);
            } else if (gtype == "int" || gtype == "uint") {
                u.type = gui::ShaderLabUniform::Type::Int;
                u.min_val = hmin;
                u.max_val = (hr != std::string::npos) ? hmax : 16.0f;
                u.step = (hr != std::string::npos) ? hstep : 1.0f;
                u.int_val = u.default_int = (nvals > 0) ? static_cast<int>(vals[0]) : 0;
            } else if (gtype == "vec3") {
                u.type = gui::ShaderLabUniform::Type::Color3;
                float def_v = (nvals == 1) ? vals[0] : 1.0f;
                for (int k = 0; k < 3; ++k) {
                    u.color_val[k] = u.default_color[k] = (k < nvals) ? vals[k] : def_v;
                }
                u.color_val[3] = u.default_color[3] = 1.0f;
            } else if (gtype == "vec4") {
                u.type = gui::ShaderLabUniform::Type::Color4;
                float def_v = (nvals == 1) ? vals[0] : 1.0f;
                for (int k = 0; k < 4; ++k) {
                    u.color_val[k] = u.default_color[k] = (k < nvals) ? vals[k] : def_v;
                }
            } else {
                u.type = gui::ShaderLabUniform::Type::Float;
                u.min_val = hmin;
                u.max_val = (hr != std::string::npos) ? hmax : std::max(2.0f, nvals > 0 ? vals[0] * 2.0f + 1.0f : 1.0f);
                u.step = hstep;
                u.float_val = u.default_float = (nvals > 0) ? vals[0] : hmin;
            }

            ParsedUniformInfo meta;
            meta.name = gname;
            meta.hlsl_type = hlsl_t;
            meta.ui_uniform_index = static_cast<int>(out_effect.uniforms.size());
            out_effect.uniforms.push_back(std::move(u));
            out_prog.uniforms_meta.push_back(std::move(meta));
            continue;
        }

        if (is_ident_start(cleaned[i])) {
            while (i < cleaned.size() && is_ident_char(cleaned[i])) {
                body_ss << cleaned[i++];
            }
        } else {
            body_ss << cleaned[i++];
        }
    }

    std::string glsl = body_ss.str();
    // Token-level GLSL -> HLSL translation
    std::string hlsl;
    hlsl.reserve(glsl.size());
    for (size_t k = 0; k < glsl.size();) {
        if (glsl[k] == '"') {
            hlsl.push_back(glsl[k++]);
            while (k < glsl.size()) {
                char c = glsl[k++];
                hlsl.push_back(c);
                if (c == '\\' && k < glsl.size()) hlsl.push_back(glsl[k++]);
                else if (c == '"') break;
            }
        } else if (is_ident_start(glsl[k])) {
            size_t st = k++;
            while (k < glsl.size() && is_ident_char(glsl[k])) ++k;
            std::string tok = glsl.substr(st, k - st);
            if (tok == "vec2") hlsl += "float2";
            else if (tok == "vec3") hlsl += "float3";
            else if (tok == "vec4") hlsl += "float4";
            else if (tok == "ivec2") hlsl += "int2";
            else if (tok == "ivec3") hlsl += "int3";
            else if (tok == "ivec4") hlsl += "int4";
            else if (tok == "uvec2") hlsl += "uint2";
            else if (tok == "uvec3") hlsl += "uint3";
            else if (tok == "uvec4") hlsl += "uint4";
            else if (tok == "bvec2") hlsl += "bool2";
            else if (tok == "bvec3") hlsl += "bool3";
            else if (tok == "bvec4") hlsl += "bool4";
            else if (tok == "mat2") hlsl += "float2x2";
            else if (tok == "mat3") hlsl += "float3x3";
            else if (tok == "mat4") hlsl += "float4x4";
            else if (tok == "mix") hlsl += "lerp";
            else if (tok == "fract") hlsl += "frac";
            else if (tok == "inversesqrt") hlsl += "rsqrt";
            else if (tok == "dFdx") hlsl += "ddx";
            else if (tok == "dFdy") hlsl += "ddy";
            else if (tok == "const") hlsl += "static const";
            else if (tok == "fragment") hlsl += "__sl_godot_fragment_body";
            else hlsl += tok;
        } else {
            hlsl.push_back(glsl[k++]);
        }
    }

    hlsl = expand_scalar_vec_ctors(hlsl);

    // Rewrite `texture(TEX, uv)` and `textureLod(TEX, uv, lod)` calls
    auto rewrite_tex_fn = [](const std::string &in, const char *fn_name, bool is_lod) {
        std::string out;
        size_t pos = 0;
        while (pos < in.size()) {
            if (starts_with_word(in, pos, fn_name)) {
                size_t lp = skip_ws_and_comments(in, pos + std::strlen(fn_name));
                if (lp < in.size() && in[lp] == '(') {
                    size_t rp = find_matching_delim(in, lp, '(', ')');
                    if (rp != std::string::npos) {
                        auto args = split_top_level_args(in.substr(lp + 1, rp - lp - 1));
                        if (args.size() >= (is_lod ? 3u : 2u)) {
                            out += "(" + args[0] + (is_lod ? ".SampleLevel(" : ".Sample(") +
                                   "__sl_godot_smp, (" + args[1] + ")" +
                                   (is_lod ? (", (" + args[2] + ")") : std::string()) + "))";
                            pos = rp + 1;
                            continue;
                        }
                    }
                }
            }
            out.push_back(in[pos++]);
        }
        return out;
    };
    hlsl = rewrite_tex_fn(hlsl, "texture", false);
    hlsl = rewrite_tex_fn(hlsl, "textureLod", true);

    std::ostringstream full;
    full << R"HLSL(
float4 __sl_system_viewport;
float4 __sl_system_time;
int __sl_framecount_i;
Texture2D __sl_godot_tex;
Texture2D __sl_godot_depth_tex;
SamplerState __sl_godot_smp;

#define TEXTURE __sl_godot_tex
#define SCREEN_TEXTURE __sl_godot_tex
#define DEPTH_TEXTURE __sl_godot_depth_tex
#define TEXTURE_PIXEL_SIZE (__sl_system_viewport.zw)
#define SCREEN_PIXEL_SIZE (__sl_system_viewport.zw)
#define TIME (__sl_system_time.w)
#define PI 3.14159265359
#define mod(x, y) ((x) - (y) * floor((x) / (y)))

float __sl_atan_poly(float y) { return atan(y); }
float __sl_atan_poly(float y, float x) { return atan2(y, x); }
#define atan __sl_atan_poly

float2 __sl_mkf2(float s) { return float2(s, s); }
float2 __sl_mkf2(float x, float y) { return float2(x, y); }
float3 __sl_mkf3(float s) { return float3(s, s, s); }
float3 __sl_mkf3(float2 v, float z) { return float3(v, z); }
float3 __sl_mkf3(float x, float2 v) { return float3(x, v); }
float3 __sl_mkf3(float x, float y, float z) { return float3(x, y, z); }
float4 __sl_mkf4(float s) { return float4(s, s, s, s); }
float4 __sl_mkf4(float3 v, float w) { return float4(v, w); }
float4 __sl_mkf4(float2 a, float2 b) { return float4(a, b); }

static float2 UV;
static float2 SCREEN_UV;
static float4 FRAGCOORD;
static float4 COLOR;
)HLSL";

    // Fix parameterless `void __sl_godot_fragment_body()` references to globals cleanly in D3D11
    // Note: module-level `static` variables mutated in void fragment() compile validly in FXC PS!
    full << "\n" << hlsl << "\n";
    full << R"HLSL(
void __ShaderLabGodotVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 __ShaderLabGodotPS(float4 pos : SV_Position, float2 in_uv : TEXCOORD0) : SV_Target
{
    UV = in_uv;
    SCREEN_UV = in_uv;
    FRAGCOORD = pos;
    COLOR = __sl_godot_tex.Sample(__sl_godot_smp, in_uv);
    __sl_godot_fragment_body();
    return COLOR;
}
)HLSL";

    out_prog.hlsl_body = full.str();
    ParsedPassDecl pass;
    pass.pass_name = "GodotCanvasPass";
    pass.vs_entry = "__ShaderLabGodotVS";
    pass.ps_entry = "__ShaderLabGodotPS";
    out_prog.passes.push_back(std::move(pass));
    (void)buf_w;
    (void)buf_h;
    (void)warnings;
    return true;
}

// -------------------- D3D11 Compile & Reflection --------------------

bool compile_hlsl_blob(
    const std::string &hlsl,
    const std::string &entry,
    const std::string &target,
    ComPtr<ID3DBlob> &out_blob,
    std::string &out_error)
{
    ComPtr<ID3DBlob> err_blob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(
        hlsl.data(),
        hlsl.size(),
        "ShaderLabDynamic",
        nullptr,
        nullptr,
        entry.c_str(),
        target.c_str(),
        flags,
        0,
        &out_blob,
        &err_blob
    );
    if (FAILED(hr)) {
        if (err_blob) {
            out_error = std::string(static_cast<const char *>(err_blob->GetBufferPointer()), err_blob->GetBufferSize());
        } else {
            out_error = "D3DCompile failed with HRESULT 0x" + std::to_string(static_cast<unsigned long>(hr));
        }
        return false;
    }
    return true;
}

void reflect_pass_resources(
    ID3D11Device *device,
    ID3DBlob *ps_blob,
    const ParsedShaderProgram &prog,
    CompiledShaderPass &out_pass,
    const CompiledEffectRuntime &runtime)
{
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), IID_PPV_ARGS(&refl)))) {
        return;
    }

    D3D11_SHADER_DESC sdesc{};
    refl->GetDesc(&sdesc);

    // Check PS SV_Target0 output mask to detect float3/float2/float shaders missing alpha
    out_pass.ps_outputs_alpha = false;
    for (UINT o = 0; o < sdesc.OutputParameters; ++o) {
        D3D11_SIGNATURE_PARAMETER_DESC odesc{};
        if (SUCCEEDED(refl->GetOutputParameterDesc(o, &odesc))) {
            std::string sem = odesc.SemanticName ? to_lower_copy(odesc.SemanticName) : std::string();
            if ((sem == "sv_target" || sem == "color") && odesc.SemanticIndex == 0) {
                if ((odesc.Mask & 0x8) != 0) {
                    out_pass.ps_outputs_alpha = true;
                }
            }
        }
    }

    // Build lookup maps
    std::unordered_map<std::string, int> rt_index_map;
    for (size_t i = 0; i < runtime.custom_rts.size(); ++i) {
        rt_index_map[runtime.custom_rts[i].spec.name] = static_cast<int>(i);
    }
    std::unordered_map<std::string, const ParsedSamplerDecl *> sampler_map;
    for (const auto &s : prog.samplers) {
        sampler_map[s.name] = &s;
    }
    std::unordered_map<std::string, const ParsedUniformInfo *> uniform_map;
    for (const auto &u : prog.uniforms_meta) {
        uniform_map[u.name] = &u;
    }

    // Discover resource bind slots (`tN`, `sN`, `bN`)
    std::vector<std::pair<uint32_t, std::string>> cbuffer_slots;
    for (UINT r = 0; r < sdesc.BoundResources; ++r) {
        D3D11_SHADER_INPUT_BIND_DESC bdesc{};
        if (FAILED(refl->GetResourceBindingDesc(r, &bdesc))) continue;
        std::string rname = bdesc.Name ? bdesc.Name : "";

        if (bdesc.Type == D3D_SIT_CBUFFER) {
            cbuffer_slots.emplace_back(bdesc.BindPoint, rname);
        } else if (bdesc.Type == D3D_SIT_TEXTURE) {
            ReflectedTextureSlot tslot;
            tslot.bind_point = bdesc.BindPoint;
            tslot.hlsl_name = rname;
            tslot.kind = ReflectedTextureSlot::SourceKind::MainInput;

            std::string stripped = rname;
            if (stripped.rfind("__sl_tex_", 0) == 0) stripped = stripped.substr(9);
            std::string slow = to_lower_copy(stripped);

            auto rtit = rt_index_map.find(stripped);
            if (rtit != rt_index_map.end()) {
                tslot.kind = ReflectedTextureSlot::SourceKind::CustomRenderTarget;
                tslot.custom_rt_index = rtit->second;
            } else if (slow.find("depth") != std::string::npos || slow == "zbuffer") {
                tslot.kind = ReflectedTextureSlot::SourceKind::DepthBuffer;
            } else {
                tslot.kind = ReflectedTextureSlot::SourceKind::MainInput;
            }
            out_pass.texture_slots.push_back(std::move(tslot));
        } else if (bdesc.Type == D3D_SIT_SAMPLER) {
            ReflectedSamplerSlot sslot;
            sslot.bind_point = bdesc.BindPoint;
            sslot.hlsl_name = rname;
            std::string stripped = rname;
            if (stripped.rfind("__sl_smp_", 0) == 0) stripped = stripped.substr(9);
            auto sit = sampler_map.find(stripped);
            if (sit != sampler_map.end()) {
                sslot.filter_mode = sit->second->filter_mode;
                sslot.address_u = sit->second->address_u;
                sslot.address_v = sit->second->address_v;
            } else if (to_lower_copy(rname).find("point") != std::string::npos) {
                sslot.filter_mode = 1;
            }
            out_pass.sampler_slots.push_back(std::move(sslot));
        }
    }

    // Reflect every bound cbuffer variables into exact byte offsets
    for (const auto &cb_entry : cbuffer_slots) {
        ID3D11ShaderReflectionConstantBuffer *cb = refl->GetConstantBufferByName(cb_entry.second.c_str());
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC cbdesc{};
        if (FAILED(cb->GetDesc(&cbdesc)) || cbdesc.Size == 0) continue;

        ReflectedCBuffer rcb;
        rcb.bind_point = cb_entry.first;
        rcb.byte_size = (cbdesc.Size + 15u) & ~15u;

        for (UINT v = 0; v < cbdesc.Variables; ++v) {
            ID3D11ShaderReflectionVariable *var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vdesc{};
            if (!var || FAILED(var->GetDesc(&vdesc)) || !vdesc.Name) continue;

            ReflectedUniformBinding ub;
            ub.var_name = vdesc.Name;
            ub.byte_offset = vdesc.StartOffset;
            ub.byte_size = vdesc.Size;

            ID3D11ShaderReflectionType *vtype = var->GetType();
            D3D11_SHADER_TYPE_DESC tdesc{};
            if (vtype && SUCCEEDED(vtype->GetDesc(&tdesc))) {
                ub.is_int = (tdesc.Type == D3D_SVT_INT);
                ub.is_uint = (tdesc.Type == D3D_SVT_UINT);
                ub.is_bool = (tdesc.Type == D3D_SVT_BOOL);
            }

            if (ub.var_name == "__sl_system_viewport") {
                ub.system_semantic = SystemUniformSemantic::BufferSize;
            } else if (ub.var_name == "__sl_system_time") {
                ub.system_semantic = SystemUniformSemantic::TimerMs;
            } else if (ub.var_name == "__sl_framecount_i") {
                ub.system_semantic = SystemUniformSemantic::FrameCount;
            } else {
                auto uit = uniform_map.find(ub.var_name);
                if (uit != uniform_map.end()) {
                    ub.ui_uniform_index = uit->second->ui_uniform_index;
                    ub.system_semantic = uit->second->system_semantic;
                }
            }

            rcb.bindings.push_back(std::move(ub));
        }

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = rcb.byte_size;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(device->CreateBuffer(&cbd, nullptr, &rcb.buffer))) {
            out_pass.cbuffers.push_back(std::move(rcb));
        }
    }
}

} // namespace

// -------------------- CompiledEffectRuntime --------------------

bool CompiledEffectRuntime::ensure_custom_rts(ID3D11Device *device, uint32_t main_w, uint32_t main_h) {
    if (!device || main_w == 0 || main_h == 0) return false;

    for (auto &rt : custom_rts) {
        uint32_t target_w = rt.spec.explicit_width > 0
            ? rt.spec.explicit_width
            : std::max(1u, static_cast<uint32_t>(std::llround(main_w * rt.spec.width_scale)));
        uint32_t target_h = rt.spec.explicit_height > 0
            ? rt.spec.explicit_height
            : std::max(1u, static_cast<uint32_t>(std::llround(main_h * rt.spec.height_scale)));

        if (rt.texture && rt.rtv && rt.srv && rt.cur_width == target_w && rt.cur_height == target_h) {
            continue;
        }

        rt.texture.Reset();
        rt.rtv.Reset();
        rt.srv.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = target_w;
        td.Height = target_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = rt.spec.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &rt.texture))) return false;
        if (FAILED(device->CreateRenderTargetView(rt.texture.Get(), nullptr, &rt.rtv))) return false;
        if (FAILED(device->CreateShaderResourceView(rt.texture.Get(), nullptr, &rt.srv))) return false;
        rt.cur_width = target_w;
        rt.cur_height = target_h;
    }
    return true;
}

// -------------------- UniversalHlslParser --------------------

ShaderDialect UniversalHlslParser::detect_dialect(const std::filesystem::path &path, const std::string &source) {
    std::string ext = to_lower_copy(path.extension().string());
    if (ext == ".fx" || ext == ".fxh") return ShaderDialect::ReShadeFx;
    if (ext == ".shader") return ShaderDialect::UnityShader;
    if (ext == ".gdshader") return ShaderDialect::GodotGdShader;
    if (ext == ".hlsl") return ShaderDialect::RawHlsl;

    if (source.find("shader_type ") != std::string::npos) return ShaderDialect::GodotGdShader;
    if (source.find("CGPROGRAM") != std::string::npos || source.find("HLSLPROGRAM") != std::string::npos) {
        return ShaderDialect::UnityShader;
    }
    if (source.find("technique ") != std::string::npos) return ShaderDialect::ReShadeFx;
    return ShaderDialect::RawHlsl;
}

bool UniversalHlslParser::parse_and_compile_shader_file(
    ID3D11Device *device,
    const std::filesystem::path &path,
    uint32_t initial_width,
    uint32_t initial_height,
    gui::ShaderLabEffect &out_effect,
    CompiledEffectRuntime &out_runtime,
    std::string &out_error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        out_error = "Failed to open shader file: " + gui::to_utf8(path.wstring());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();
    ShaderDialect dialect = detect_dialect(path, source);

    return parse_and_compile_source(
        device,
        source,
        path,
        dialect,
        initial_width,
        initial_height,
        out_effect,
        out_runtime,
        out_error
    );
}

bool UniversalHlslParser::parse_and_compile_source(
    ID3D11Device *device,
    const std::string &source,
    const std::filesystem::path &virtual_path,
    ShaderDialect dialect,
    uint32_t initial_width,
    uint32_t initial_height,
    gui::ShaderLabEffect &out_effect,
    CompiledEffectRuntime &out_runtime,
    std::string &out_error)
{
    if (!device) {
        out_error = "D3D11 device is null";
        return false;
    }
    if (dialect == ShaderDialect::Unknown) {
        dialect = detect_dialect(virtual_path, source);
    }

    out_effect = gui::ShaderLabEffect{};
    out_effect.name = virtual_path.stem().string();
    if (out_effect.name.empty()) out_effect.name = "Custom Shader";
    out_effect.file_path = gui::to_utf8(virtual_path.wstring());
    out_effect.enabled = true;

    out_runtime = CompiledEffectRuntime{};
    out_runtime.effect_name = out_effect.name;
    out_runtime.file_path = out_effect.file_path;
    out_runtime.dialect = dialect;

    switch (dialect) {
        case ShaderDialect::ReShadeFx: out_effect.engine_type = "ReShade"; break;
        case ShaderDialect::UnityShader: out_effect.engine_type = "Unity"; break;
        case ShaderDialect::GodotGdShader: out_effect.engine_type = "Godot"; break;
        default: out_effect.engine_type = "HLSL"; break;
    }
    out_runtime.engine_type = out_effect.engine_type;

    ParsedShaderProgram prog;
    bool parsed = false;

    if (dialect == ShaderDialect::ReShadeFx || dialect == ShaderDialect::RawHlsl) {
        Preprocessor pp(virtual_path, initial_width, initial_height, out_runtime.warnings);
        std::string preprocessed = pp.run(source, virtual_path);
        parsed = parse_reshade_or_hlsl_program(
            preprocessed,
            dialect,
            initial_width,
            initial_height,
            out_effect,
            prog,
            out_runtime.warnings
        );
    } else if (dialect == ShaderDialect::UnityShader) {
        parsed = parse_unity_shader_program(
            source,
            initial_width,
            initial_height,
            out_effect,
            prog,
            out_runtime.warnings
        );
    } else if (dialect == ShaderDialect::GodotGdShader) {
        parsed = parse_godot_gdshader_program(
            source,
            initial_width,
            initial_height,
            out_effect,
            prog,
            out_runtime.warnings
        );
    }

    if (!parsed || prog.passes.empty()) {
        out_error = out_runtime.warnings.empty()
            ? "Failed to parse shader structure."
            : out_runtime.warnings.front();
        return false;
    }

    // Build custom render target list for non-backbuffer/non-depth textures
    std::unordered_map<std::string, int> rt_lookup;
    for (const auto &td : prog.textures) {
        if (!td.is_backbuffer && !td.is_depth) {
            CustomRenderTarget crt;
            crt.spec = td.rt_spec;
            rt_lookup[td.name] = static_cast<int>(out_runtime.custom_rts.size());
            out_runtime.custom_rts.push_back(std::move(crt));
        }
    }

    if (initial_width > 0 && initial_height > 0) {
        out_runtime.ensure_custom_rts(device, initial_width, initial_height);
    }

    // Compile each pass to D3D11 SM 5.0 shaders and reflect uniforms/resources
    for (const auto &pdecl : prog.passes) {
        CompiledShaderPass pass;
        pass.pass_name = pdecl.pass_name;
        pass.clear_rt = pdecl.clear_rt;
        pass.srgb_write = pdecl.srgb_write;

        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        std::string compile_err;
        if (!compile_hlsl_blob(prog.hlsl_body, pdecl.vs_entry, "vs_5_0", vs_blob, compile_err)) {
            out_error = "Vertex shader '" + pdecl.vs_entry + "' compile error:\n" + compile_err;
            return false;
        }
        if (!compile_hlsl_blob(prog.hlsl_body, pdecl.ps_entry, "ps_5_0", ps_blob, compile_err)) {
            out_error = "Pixel shader '" + pdecl.ps_entry + "' compile error:\n" + compile_err;
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &pass.vertex_shader))) {
            out_error = "Failed to create ID3D11VertexShader for pass " + pdecl.pass_name;
            return false;
        }
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pass.pixel_shader))) {
            out_error = "Failed to create ID3D11PixelShader for pass " + pdecl.pass_name;
            return false;
        }

        for (const auto &rt_name : pdecl.render_targets) {
            auto it = rt_lookup.find(rt_name);
            if (it != rt_lookup.end()) {
                pass.output_rt_indices.push_back(it->second);
            }
        }
        pass.writes_to_main = pass.output_rt_indices.empty();

        reflect_pass_resources(device, ps_blob.Get(), prog, pass, out_runtime);
        out_runtime.passes.push_back(std::move(pass));
    }

    out_runtime.valid = true;
    return true;
}

} // namespace shaderlab
