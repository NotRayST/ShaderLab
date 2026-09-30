#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <unordered_map>
#include "../../../common/str_utils.h"

namespace fs = std::filesystem;
using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

struct ShaderErrorInfo {
    std::string file;
    int line = 0;
    int column = 0;
    std::string code;
    std::string message;
    bool is_warning = false;
};

inline fs::path find_reshade_ini_path() {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    fs::path ini_path = fs::path(exe_path).parent_path() / "ReShade.ini";
    if (fs::exists(ini_path)) return ini_path;
    if (fs::exists("ReShade.ini")) return fs::path("ReShade.ini");
    return ini_path;
}

inline std::vector<fs::path> get_reshade_log_candidates(const fs::path &shader_or_ini_path = {}) {
    std::vector<fs::path> candidates;
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        candidates.push_back(fs::path(exe_path).parent_path() / "ReShade.log");
    }
    candidates.push_back(fs::current_path() / "ReShade.log");
    if (!shader_or_ini_path.empty()) {
        candidates.push_back(shader_or_ini_path.parent_path() / "ReShade.log");
    }
    return candidates;
}

inline void clear_reshade_log_files(const fs::path &shader_or_ini_path = {}) {
    auto candidates = get_reshade_log_candidates(shader_or_ini_path);
    std::error_code ec;
    for (const auto &p : candidates) {
        if (fs::exists(p)) {
            fs::remove(p, ec);
        }
    }
}

inline std::string read_reshade_log(const fs::path &shader_or_ini_path = {}) {
    auto candidates = get_reshade_log_candidates(shader_or_ini_path);
    for (const auto &p : candidates) {
        if (fs::exists(p)) {
            std::ifstream in(p, std::ios::binary);
            if (in.is_open()) {
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!content.empty()) return content;
            }
        }
    }
    return {};
}

inline std::vector<ShaderErrorInfo> parse_reshade_log_errors(const std::string &log_content, const std::string &target_filename = {}) {
    std::vector<ShaderErrorInfo> errors;
    std::stringstream ss(log_content);
    std::string line;

    std::string target_lower = target_filename;
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

    std::regex re_err(R"((.+?)\((\d+)(?:,\s*(\d+))?\):\s*(error|warning)\s*([A-Za-z0-9]+)?:\s*(.+))", std::regex::icase);

    while (std::getline(ss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re_err)) {
            std::string file_in_log = m[1].str();
            std::string file_lower = file_in_log;
            std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

            if (!target_lower.empty() && file_lower.find(target_lower) == std::string::npos) {
                continue;
            }

            std::string l_str = m[2].str();
            std::string c_str = m[3].matched ? m[3].str() : "0";
            std::string severity = m[4].str();
            std::string code = m[5].matched ? m[5].str() : "";
            std::string msg = m[6].str();

            std::transform(severity.begin(), severity.end(), severity.begin(), [](unsigned char c) { return (char)::tolower(c); });

            ShaderErrorInfo info;
            info.file = file_in_log;
            info.line = std::stoi(l_str);
            info.column = std::stoi(c_str);
            info.code = code;
            info.message = msg;
            info.is_warning = (severity == "warning");

            errors.push_back(info);
        }
    }
    return errors;
}

inline bool check_shader_compile_in_log(
    const std::string &log_content,
    const std::string &target_filename,
    bool &out_failed,
    bool &out_succeeded,
    std::string &out_error_summary)
{
    out_failed = false;
    out_succeeded = false;
    out_error_summary.clear();

    if (log_content.empty()) return false;

    std::string fn_lower = target_filename;
    std::transform(fn_lower.begin(), fn_lower.end(), fn_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

    std::string log_lower = log_content;
    std::transform(log_lower.begin(), log_lower.end(), log_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

    bool target_mentioned = fn_lower.empty() || (log_lower.find(fn_lower) != std::string::npos);

    if (!target_mentioned) {
        return false;
    }

    auto errors = parse_reshade_log_errors(log_content, target_filename);
    bool has_compile_errors = false;
    for (const auto &e : errors) {
        if (!e.is_warning) {
            has_compile_errors = true;
            break;
        }
    }

    bool failed_logged = false;
    if (fn_lower.empty()) {
        failed_logged = (log_content.find("Failed to compile") != std::string::npos);
    } else {
        std::stringstream ss(log_content);
        std::string line;
        while (std::getline(ss, line)) {
            std::string l_lower = line;
            std::transform(l_lower.begin(), l_lower.end(), l_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });
            if (l_lower.find("failed to compile") != std::string::npos && l_lower.find(fn_lower) != std::string::npos) {
                failed_logged = true;
                break;
            }
        }
    }

    bool success_logged = false;
    if (fn_lower.empty()) {
        success_logged = (log_content.find("Successfully compiled") != std::string::npos);
    } else {
        std::stringstream ss(log_content);
        std::string line;
        while (std::getline(ss, line)) {
            std::string l_lower = line;
            std::transform(l_lower.begin(), l_lower.end(), l_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });
            if (l_lower.find("successfully compiled") != std::string::npos && l_lower.find(fn_lower) != std::string::npos) {
                success_logged = true;
                break;
            }
        }
    }

    if (failed_logged || has_compile_errors) {
        out_failed = true;
        std::stringstream err_ss;
        for (const auto &e : errors) {
            err_ss << "  " << e.file << "(" << e.line << "," << e.column << "): "
                   << (e.is_warning ? "[WARNING] " : "[ERROR] ")
                   << e.code << ": " << e.message << "\n";
        }
        out_error_summary = err_ss.str();
        return true;
    }

    if (success_logged) {
        out_succeeded = true;
        return true;
    }

    return false;
}

inline bool is_folder_covered_by_search_paths(const fs::path &folder, const std::string &search_paths_str, const fs::path &base_dir) {
    if (folder.empty()) return false;

    std::error_code ec;
    fs::path norm_target = fs::weakly_canonical(folder, ec);
    if (ec) norm_target = fs::absolute(folder);
    std::wstring target_str = norm_target.wstring();
    std::transform(target_str.begin(), target_str.end(), target_str.begin(), ::towlower);
    while (!target_str.empty() && (target_str.back() == L'\\' || target_str.back() == L'/')) {
        target_str.pop_back();
    }

    std::stringstream ss(search_paths_str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t first = token.find_first_not_of(" \t\r\n\"");
        if (first == std::string::npos) continue;
        size_t last = token.find_last_not_of(" \t\r\n\"");
        std::string path_entry = token.substr(first, last - first + 1);
        if (path_entry.empty()) continue;

        bool is_recursive = false;
        if (path_entry.size() >= 3 && path_entry.substr(path_entry.size() - 3) == "\\**") {
            is_recursive = true;
            path_entry = path_entry.substr(0, path_entry.size() - 3);
        } else if (path_entry.size() >= 3 && path_entry.substr(path_entry.size() - 3) == "/**") {
            is_recursive = true;
            path_entry = path_entry.substr(0, path_entry.size() - 3);
        } else if (path_entry.size() >= 2 && path_entry.substr(path_entry.size() - 2) == "\\*") {
            is_recursive = true;
            path_entry = path_entry.substr(0, path_entry.size() - 2);
        } else if (path_entry.size() >= 2 && path_entry.substr(path_entry.size() - 2) == "/*") {
            is_recursive = true;
            path_entry = path_entry.substr(0, path_entry.size() - 2);
        } else if (path_entry.size() >= 2 && path_entry.substr(path_entry.size() - 2) == "**") {
            is_recursive = true;
            path_entry = path_entry.substr(0, path_entry.size() - 2);
        }

        fs::path entry_path = path_entry;
        if (entry_path.is_relative() && !base_dir.empty()) {
            entry_path = base_dir / entry_path;
        }

        fs::path norm_entry = fs::weakly_canonical(entry_path, ec);
        if (ec) norm_entry = fs::absolute(entry_path);
        std::wstring entry_str = norm_entry.wstring();
        std::transform(entry_str.begin(), entry_str.end(), entry_str.begin(), ::towlower);
        while (!entry_str.empty() && (entry_str.back() == L'\\' || entry_str.back() == L'/')) {
            entry_str.pop_back();
        }

        if (target_str == entry_str) {
            return true;
        }

        if (is_recursive) {
            if (target_str.size() > entry_str.size() &&
                target_str.rfind(entry_str, 0) == 0 &&
                (target_str[entry_str.size()] == L'\\' || target_str[entry_str.size()] == L'/')) {
                return true;
            }
        }
    }

    return false;
}

class ReShadeIniGuard {
public:
    explicit ReShadeIniGuard(const fs::path &ini_path) : m_ini_path(ini_path) {
        m_had_original = fs::exists(m_ini_path);
        if (m_had_original) {
            std::ifstream in(m_ini_path, std::ios::binary);
            if (in.is_open()) {
                m_original_content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
        }
    }

    ~ReShadeIniGuard() {
        restore();
    }

    void restore() {
        if (m_restored) return;
        m_restored = true;
        if (m_had_original) {
            std::ofstream out(m_ini_path, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(m_original_content.data(), m_original_content.size());
            }
        } else {
            std::error_code ec;
            fs::remove(m_ini_path, ec);
        }
    }

private:
    fs::path m_ini_path;
    bool m_had_original = false;
    bool m_restored = false;
    std::string m_original_content;
};

inline void configure_reshade_ini(
    const fs::path &ini_path,
    const fs::path &preset_path,
    const fs::path &extra_shader_dir = {})
{
    if (!fs::exists(ini_path)) {
        std::ofstream create_ini(ini_path);
        create_ini << "[GENERAL]\nPresetPath=" << fs::absolute(preset_path).string() << "\n";
        if (!extra_shader_dir.empty()) {
            create_ini << "EffectSearchPaths=.\\," << fs::absolute(extra_shader_dir).string() << "\n";
        }
        create_ini.close();
        return;
    }

    std::ifstream in(ini_path);
    if (!in.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    bool in_general = false;
    bool in_overlay = false;
    bool preset_set = false;
    bool had_general = false;
    bool had_effect_paths = false;
    std::string abs_preset = fs::absolute(preset_path).string();
    fs::path base_dir = ini_path.parent_path();

    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }

        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        if (trimmed.rfind("[", 0) == 0) {
            if (in_general) {
                if (!preset_set) {
                    lines.push_back("PresetPath=" + abs_preset);
                    preset_set = true;
                }
                if (!extra_shader_dir.empty() && !had_effect_paths) {
                    lines.push_back("EffectSearchPaths=.\\," + fs::absolute(extra_shader_dir).string());
                    had_effect_paths = true;
                }
            }
            in_general = (_stricmp(trimmed.c_str(), "[GENERAL]") == 0);
            if (in_general) had_general = true;
            in_overlay = (_stricmp(trimmed.c_str(), "[OVERLAY]") == 0);
        }

        if (in_general && trimmed.rfind("PresetPath=", 0) == 0) {
            lines.push_back("PresetPath=" + abs_preset);
            preset_set = true;
            continue;
        }

        if (in_general && trimmed.rfind("SkipLoadingDisabledEffects=", 0) == 0) {
            lines.push_back("SkipLoadingDisabledEffects=1");
            continue;
        }

        if (in_general && trimmed.rfind("EffectSearchPaths=", 0) == 0) {
            had_effect_paths = true;
            if (!extra_shader_dir.empty()) {
                std::string existing_paths = trimmed.substr(18);
                if (!is_folder_covered_by_search_paths(extra_shader_dir, existing_paths, base_dir)) {
                    line += "," + fs::absolute(extra_shader_dir).string();
                }
            }
            lines.push_back(line);
            continue;
        }

        if (in_general && trimmed.rfind("PreprocessorDefinitions=", 0) == 0) {
            std::string defs = line;
            size_t pos = 0;
            while ((pos = defs.find(",,")) != std::string::npos) {
                defs.replace(pos, 2, ",");
            }
            lines.push_back(defs);
            continue;
        }

        if (in_overlay) {
            if (trimmed.rfind("ShowFPS=", 0) == 0) { lines.push_back("ShowFPS=0"); continue; }
            if (trimmed.rfind("ShowClock=", 0) == 0) { lines.push_back("ShowClock=0"); continue; }
            if (trimmed.rfind("ShowFrameTime=", 0) == 0) { lines.push_back("ShowFrameTime=0"); continue; }
            if (trimmed.rfind("ShowPresetName=", 0) == 0) { lines.push_back("ShowPresetName=0"); continue; }
            if (trimmed.rfind("TutorialProgress=", 0) == 0) { lines.push_back("TutorialProgress=4"); continue; }
            if (trimmed.rfind("ShowPresetTransitionMessage=", 0) == 0) { lines.push_back("ShowPresetTransitionMessage=0"); continue; }
            if (trimmed.rfind("ShowScreenshotMessage=", 0) == 0) { lines.push_back("ShowScreenshotMessage=0"); continue; }
        }

        lines.push_back(line);
    }
    in.close();

    if (in_general) {
        if (!preset_set) {
            lines.push_back("PresetPath=" + abs_preset);
            preset_set = true;
        }
        if (!extra_shader_dir.empty() && !had_effect_paths) {
            lines.push_back("EffectSearchPaths=.\\," + fs::absolute(extra_shader_dir).string());
            had_effect_paths = true;
        }
    }
    if (!had_general) {
        lines.push_back("[GENERAL]");
        lines.push_back("PresetPath=" + abs_preset);
        if (!extra_shader_dir.empty()) {
            lines.push_back("EffectSearchPaths=.\\," + fs::absolute(extra_shader_dir).string());
        }
    }

    std::ofstream out(ini_path, std::ios::trunc);
    for (const auto &l : lines) {
        out << l << "\n";
    }
}

inline std::vector<std::string> extract_techniques_from_shader(const fs::path &shader_file) {
    std::vector<std::string> techniques;
    std::ifstream in(shader_file);
    if (!in.is_open()) return techniques;

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::regex tech_regex(R"(\btechnique\s+([A-Za-z0-9_]+))");
    auto words_begin = std::sregex_iterator(content.begin(), content.end(), tech_regex);
    auto words_end = std::sregex_iterator();

    for (auto it = words_begin; it != words_end; ++it) {
        std::smatch match = *it;
        std::string name = match[1].str();
        if (std::find(techniques.begin(), techniques.end(), name) == techniques.end()) {
            techniques.push_back(name);
        }
    }
    return techniques;
}

inline fs::path synthesize_shader_preset(
    const fs::path &shader_path,
    const std::wstring &technique_name,
    const std::vector<std::pair<std::string, std::string>> &uniforms)
{
    std::string filename = shader_path.filename().string();
    std::vector<std::string> techniques;
    if (!technique_name.empty()) {
        techniques.push_back(wide_to_utf8(technique_name.c_str()));
    } else {
        techniques = extract_techniques_from_shader(shader_path);
        if (techniques.empty()) {
            techniques.push_back(shader_path.stem().string());
        }
    }

    std::string tech_str;
    for (size_t i = 0; i < techniques.size(); ++i) {
        if (i > 0) tech_str += ",";
        tech_str += techniques[i] + "@" + filename;
    }

    wchar_t temp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    fs::path temp_preset = fs::path(temp_dir) / ("ShaderLab_temp_" + std::to_string(GetCurrentProcessId()) + "_" + filename + ".ini");

    std::ofstream out(temp_preset, std::ios::trunc);
    out << "Techniques=" << tech_str << "\n";
    out << "TechniqueSorting=" << tech_str << "\n\n";
    out << "[" << filename << "]\n";
    for (const auto &kv : uniforms) {
        if (kv.first.find(':') == std::string::npos) {
            out << kv.first << "=" << kv.second << "\n";
        }
    }
    for (const auto &kv : uniforms) {
        size_t colon_pos = kv.first.find(':');
        if (colon_pos != std::string::npos) {
            std::string sec = kv.first.substr(0, colon_pos);
            std::string var = kv.first.substr(colon_pos + 1);
            out << "\n[" << sec << "]\n" << var << "=" << kv.second << "\n";
        }
    }
    out.close();

    return temp_preset;
}

inline std::vector<std::string> extract_shaders_from_preset(const fs::path &preset_path) {
    std::vector<std::string> shaders;
    std::ifstream in(preset_path);
    if (!in.is_open()) return shaders;

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
        if (trimmed.rfind("Techniques=", 0) == 0) {
            std::string techs = trimmed.substr(11);
            std::stringstream ss(techs);
            std::string t;
            while (std::getline(ss, t, ',')) {
                size_t at_pos = t.find('@');
                if (at_pos != std::string::npos) {
                    std::string sh = t.substr(at_pos + 1);
                    if (!sh.empty() && std::find(shaders.begin(), shaders.end(), sh) == shaders.end()) {
                        shaders.push_back(sh);
                    }
                }
            }
            break;
        }
    }
    return shaders;
}
