#include "cli_preset.h"
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

#include "../../../common/str_utils.h"

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

static std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

struct ParsedPreset {
    std::string path;
    std::vector<std::string> techniques;
    std::vector<std::string> sorting;
    std::map<std::string, std::map<std::string, std::string>> sections;
};

static ParsedPreset parse_ini(const std::wstring &path) {
    ParsedPreset p;
    p.path = wide_to_utf8(path.c_str());

    std::ifstream in(path);
    if (!in.is_open()) return p;

    std::string line;
    std::string cur_section = "";

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            cur_section = line.substr(1, line.length() - 2);
            continue;
        }

        auto eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            p.sections[cur_section][key] = val;

            if (cur_section.empty() || cur_section == "General" || cur_section == "") {
                if (key == "Techniques") {
                    std::stringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        std::string t = trim(item);
                        if (!t.empty()) p.techniques.push_back(t);
                    }
                } else if (key == "TechniqueSorting") {
                    std::stringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        std::string t = trim(item);
                        if (!t.empty()) p.sorting.push_back(t);
                    }
                }
            }
        }
    }
    return p;
}

int CliPreset::execute(const CliOptions &opts) {
    if (opts.preset_path.empty()) {
        std::cerr << "Error: No preset specified.\n";
        std::cerr << "Usage: ShaderLab.exe preset <preset.ini> [--diff <preset2.ini>] [--json]\n";
        return 1;
    }

    if (!fs::exists(opts.preset_path)) {
        std::cerr << "Error: Preset file not found: " << wide_to_utf8(opts.preset_path.c_str()) << "\n";
        return 1;
    }

    ParsedPreset p1 = parse_ini(opts.preset_path);

    // diff if two presets given
    if (!opts.preset_path_b.empty()) {
        if (!fs::exists(opts.preset_path_b)) {
            std::cerr << "Error: Second preset file not found: " << wide_to_utf8(opts.preset_path_b.c_str()) << "\n";
            return 1;
        }

        ParsedPreset p2 = parse_ini(opts.preset_path_b);

        std::set<std::string> t1_set(p1.techniques.begin(), p1.techniques.end());
        std::set<std::string> t2_set(p2.techniques.begin(), p2.techniques.end());

        std::vector<std::string> added_techs, removed_techs, shared_techs;
        for (const auto &t : p2.techniques) {
            if (t1_set.find(t) == t1_set.end()) added_techs.push_back(t);
            else shared_techs.push_back(t);
        }
        for (const auto &t : p1.techniques) {
            if (t2_set.find(t) == t2_set.end()) removed_techs.push_back(t);
        }

        struct ParamDiff {
            std::string section;
            std::string key;
            std::string val_a;
            std::string val_b;
        };
        std::vector<ParamDiff> changed_params;

        for (const auto &[sec, kv_map] : p2.sections) {
            auto it_sec = p1.sections.find(sec);
            for (const auto &[k, val_b] : kv_map) {
                if (k == "Techniques" || k == "TechniqueSorting") continue;
                if (it_sec != p1.sections.end()) {
                    auto it_k = it_sec->second.find(k);
                    if (it_k != it_sec->second.end()) {
                        if (it_k->second != val_b) {
                            changed_params.push_back({sec, k, it_k->second, val_b});
                        }
                    } else {
                        changed_params.push_back({sec, k, "<undefined>", val_b});
                    }
                } else {
                    changed_params.push_back({sec, k, "<undefined>", val_b});
                }
            }
        }

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"preset_a\": \"" << json_escape(p1.path) << "\",\n"
                      << "  \"preset_b\": \"" << json_escape(p2.path) << "\",\n"
                      << "  \"added_techniques\": [";
            for (size_t i = 0; i < added_techs.size(); ++i) {
                std::cout << "\"" << json_escape(added_techs[i]) << "\"" << (i + 1 < added_techs.size() ? ", " : "");
            }
            std::cout << "],\n  \"removed_techniques\": [";
            for (size_t i = 0; i < removed_techs.size(); ++i) {
                std::cout << "\"" << json_escape(removed_techs[i]) << "\"" << (i + 1 < removed_techs.size() ? ", " : "");
            }
            std::cout << "],\n  \"changed_parameters\": [\n";
            for (size_t i = 0; i < changed_params.size(); ++i) {
                const auto &cd = changed_params[i];
                std::cout << "    {\"section\": \"" << json_escape(cd.section) << "\", \"key\": \"" << json_escape(cd.key)
                          << "\", \"val_a\": \"" << json_escape(cd.val_a) << "\", \"val_b\": \"" << json_escape(cd.val_b) << "\"}"
                          << (i + 1 < changed_params.size() ? ",\n" : "\n");
            }
            std::cout << "  ]\n}\n";
            return 0;
        }

        std::cout << "========================================\n"
                  << "  ReShade Preset Comparison Diff\n"
                  << "========================================\n"
                  << "Preset A: " << p1.path << "\n"
                  << "Preset B: " << p2.path << "\n\n";

        std::cout << "--- Technique Changes ---\n";
        for (const auto &t : added_techs)   std::cout << "  + [ENABLED]  " << t << "\n";
        for (const auto &t : removed_techs) std::cout << "  - [DISABLED] " << t << "\n";
        if (added_techs.empty() && removed_techs.empty()) {
            std::cout << "  (Identical active technique set)\n";
        }

        std::cout << "\n--- Parameter & Uniform Changes (" << changed_params.size() << ") ---\n";
        for (const auto &cd : changed_params) {
            std::cout << "  [" << cd.section << "] " << cd.key << ": " << cd.val_a << " -> " << cd.val_b << "\n";
        }
        std::cout << "========================================\n";
        return 0;
    }

    // single preset inspection
    if (opts.json_output) {
        std::cout << "{\n"
                  << "  \"path\": \"" << json_escape(p1.path) << "\",\n"
                  << "  \"technique_count\": " << p1.techniques.size() << ",\n"
                  << "  \"techniques\": [";
        for (size_t i = 0; i < p1.techniques.size(); ++i) {
            std::cout << "\"" << json_escape(p1.techniques[i]) << "\"" << (i + 1 < p1.techniques.size() ? ", " : "");
        }
        std::cout << "],\n  \"sections\": " << p1.sections.size() << "\n}\n";
        return 0;
    }

    std::cout << "========================================\n"
              << "  ReShade Preset Inspector\n"
              << "========================================\n"
              << "File:        " << p1.path << "\n"
              << "Techniques:  " << p1.techniques.size() << " active\n\n";

    for (size_t i = 0; i < p1.techniques.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << p1.techniques[i] << "\n";
    }

    if (p1.sections.find("PreprocessorDefinitions") != p1.sections.end()) {
        std::cout << "\nPreprocessor Definitions:\n";
        for (const auto &[k, v] : p1.sections["PreprocessorDefinitions"]) {
            std::cout << "  - " << k << " = " << v << "\n";
        }
    }

    std::cout << "========================================\n";
    return 0;
}
