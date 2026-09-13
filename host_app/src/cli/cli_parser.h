#pragma once
#include <string>
#include <vector>

struct CliOptions {
    std::string subcommand; // "render", "test-shader", "depth", "info", "preset", "help", "selftest"
    std::wstring input_path;
    std::wstring output_path;
    std::wstring preset_path;
    std::wstring preset_path_b; // for diffing
    std::wstring shader_path;
    std::string encoder = "vitl";
    int settle_frames = 45;
    float far_plane = 1000.0f;
    bool embed_png = false;
    bool sidecar = false;
    bool json_output = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
    std::string format = "png";
    std::wstring technique_name;
    std::vector<std::pair<std::string, std::string>> uniform_overrides;
    std::vector<std::wstring> extra_args;
};

class CliParser {
public:
    static bool is_cli_invocation(int argc, wchar_t **argv);
    static CliOptions parse(int argc, wchar_t **argv);
    static void print_help(const std::string &subcommand = "");
    static void print_version();
};

// escape string for valid json output formatting
inline std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

