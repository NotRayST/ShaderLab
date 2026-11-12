#include "project_file.h"
#include "../third_party/miniz/miniz.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace project_file {

#pragma pack(push, 1)
struct ZipLocalHeader {
    uint32_t signature = 0x04034b50;
    uint16_t version_needed = 20;
    uint16_t flags = 0;
    uint16_t compression = 0; // 0 = Stored (fast, no recompression on images)
    uint16_t mod_time = 0;
    uint16_t mod_date = 0;
    uint32_t crc32 = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint16_t name_len = 0;
    uint16_t extra_len = 0;
};

struct ZipCentralHeader {
    uint32_t signature = 0x02014b50;
    uint16_t version_made_by = 20;
    uint16_t version_needed = 20;
    uint16_t flags = 0;
    uint16_t compression = 0;
    uint16_t mod_time = 0;
    uint16_t mod_date = 0;
    uint32_t crc32 = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint16_t name_len = 0;
    uint16_t extra_len = 0;
    uint16_t comment_len = 0;
    uint16_t disk_start = 0;
    uint16_t internal_attr = 0;
    uint32_t external_attr = 0;
    uint32_t local_header_offset = 0;
};

struct ZipEndOfCentralDir {
    uint32_t signature = 0x06054b50;
    uint16_t disk_number = 0;
    uint16_t central_dir_disk = 0;
    uint16_t records_on_disk = 0;
    uint16_t total_records = 0;
    uint32_t central_dir_size = 0;
    uint32_t central_dir_offset = 0;
    uint16_t comment_len = 0;
};
#pragma pack(pop)

struct InternalZipEntry {
    std::string name;
    uint32_t crc32 = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

static bool read_entire_file(const fs::path &p, std::vector<uint8_t> &out) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize sz = in.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    in.seekg(0, std::ios::beg);
    if (sz > 0) in.read(reinterpret_cast<char *>(out.data()), sz);
    return in.good();
}

bool is_project_file(const std::wstring &path) {
    if (path.empty()) return false;
    std::wstring ext = fs::path(path).extension().wstring();
    for (auto &c : ext) {
        if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
    }
    return ext == L".shaderlab" || ext == L".slab";
}

static std::string json_escape_str(const std::string &src) {
    std::string out;
    out.reserve(src.size() + 8);
    for (unsigned char c : src) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

static std::string json_unescape_str(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            char next = src[++i];
            switch (next) {
            case '\"': out.push_back('\"'); break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            default:   out.push_back(next); break;
            }
        } else {
            out.push_back(src[i]);
        }
    }
    return out;
}

static bool is_safe_zip_entry(const std::string &name) {
    if (name.empty()) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.find(':') != std::string::npos) return false;
    return true;
}

static std::string build_manifest_json(
    const std::string &format_version,
    const std::string &orig_img_name,
    const ViewportState &v,
    const DepthState &d,
    uint32_t canvas_width,
    uint32_t canvas_height,
    uint32_t layer_count,
    int active_layer_index
) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"format_version\": \"" << format_version << "\",\n";
    ss << "  \"app_version\": \"v2.0\",\n";
    ss << "  \"timestamp\": " << now_ms << ",\n";
    ss << "  \"original_image_name\": \"" << json_escape_str(orig_img_name) << "\",\n";
    ss << "  \"image_width\": " << canvas_width << ",\n";
    ss << "  \"image_height\": " << canvas_height << ",\n";
    ss << "  \"layer_count\": " << layer_count << ",\n";
    ss << "  \"active_layer_index\": " << active_layer_index << ",\n";
    ss << "  \"viewport\": {\n";
    ss << "    \"zoom\": " << v.zoom << ",\n";
    ss << "    \"angle\": " << v.angle << ",\n";
    ss << "    \"pan_x\": " << v.pan[0] << ",\n";
    ss << "    \"pan_y\": " << v.pan[1] << ",\n";
    ss << "    \"is_fullscreen\": " << (v.is_fullscreen ? "true" : "false") << ",\n";
    ss << "    \"lock_pan\": " << (v.lock_pan ? "true" : "false") << ",\n";
    ss << "    \"lock_zoom\": " << (v.lock_zoom ? "true" : "false") << ",\n";
    ss << "    \"lock_rot\": " << (v.lock_rot ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"depth\": {\n";
    ss << "    \"has_depth\": " << (d.has_depth ? "true" : "false") << ",\n";
    ss << "    \"far_plane\": " << d.far_plane << "\n";
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

// Finds a JSON object key outside of string values, returning index of ':' or npos.
static size_t find_json_key(const std::string &json, const std::string &key) {
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
            std::string tok = json_unescape_str(json.substr(start, end - start));
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

static float parse_float_field(const std::string &json, const std::string &key, float default_val) {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return default_val;
    try {
        return std::stof(json.substr(colon + 1));
    } catch (...) {
        return default_val;
    }
}

static int parse_int_field(const std::string &json, const std::string &key, int default_val) {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return default_val;
    try {
        return std::stoi(json.substr(colon + 1));
    } catch (...) {
        return default_val;
    }
}

static bool parse_bool_field(const std::string &json, const std::string &key, bool default_val) {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return default_val;
    size_t end = json.find_first_of(",}\n\r", colon + 1);
    std::string val_part = json.substr(colon + 1, (end == std::string::npos) ? std::string::npos : (end - (colon + 1)));
    if (val_part.find("true") != std::string::npos) return true;
    if (val_part.find("false") != std::string::npos) return false;
    return default_val;
}

static std::string parse_string_field(const std::string &json, const std::string &key, const std::string &default_val = "") {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return default_val;
    size_t q1 = colon + 1;
    while (q1 < json.size() && (json[q1] == ' ' || json[q1] == '\t' || json[q1] == '\r' || json[q1] == '\n')) {
        ++q1;
    }
    if (q1 >= json.size() || json[q1] != '\"') return default_val;
    size_t q2 = q1 + 1;
    while (q2 < json.size()) {
        if (json[q2] == '\\' && q2 + 1 < json.size()) {
            q2 += 2;
            continue;
        }
        if (json[q2] == '\"') break;
        ++q2;
    }
    if (q2 >= json.size()) return default_val;
    return json_unescape_str(json.substr(q1 + 1, q2 - q1 - 1));
}

static std::string build_layers_json(
    const std::vector<SerializedLayerEntry> &layers,
    const std::vector<std::string> &png_entries
) {
    std::ostringstream ss;
    ss << "{\n  \"layers\": [\n";
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto &L = layers[i];
        ss << "    {\n";
        ss << "      \"id\": " << L.id << ",\n";
        ss << "      \"name\": \"" << json_escape_str(L.name) << "\",\n";
        ss << "      \"type\": " << L.type << ",\n";
        ss << "      \"visible\": " << (L.visible ? "true" : "false") << ",\n";
        ss << "      \"blend_mode\": " << L.blend_mode << ",\n";
        ss << "      \"opacity\": " << L.opacity << ",\n";
        ss << "      \"fill_opacity\": " << L.fill_opacity << ",\n";
        ss << "      \"offset_x\": " << L.offset_x << ",\n";
        ss << "      \"offset_y\": " << L.offset_y << ",\n";
        ss << "      \"rotation_deg\": " << L.rotation_deg << ",\n";
        ss << "      \"scale_x\": " << L.scale_x << ",\n";
        ss << "      \"scale_y\": " << L.scale_y << ",\n";
        ss << "      \"lock_alpha\": " << (L.lock_alpha ? "true" : "false") << ",\n";
        ss << "      \"lock_pixels\": " << (L.lock_pixels ? "true" : "false") << ",\n";
        ss << "      \"lock_position\": " << (L.lock_position ? "true" : "false") << ",\n";
        ss << "      \"width\": " << L.width << ",\n";
        ss << "      \"height\": " << L.height << ",\n";
        ss << "      \"png_entry\": \"" << json_escape_str(i < png_entries.size() ? png_entries[i] : std::string()) << "\",\n";
        ss << "      \"text_utf8\": \"" << json_escape_str(L.text_utf8) << "\",\n";
        ss << "      \"font_family\": \"" << json_escape_str(L.font_family) << "\",\n";
        ss << "      \"font_size\": " << L.font_size << ",\n";
        ss << "      \"text_r\": " << L.text_color[0] << ",\n";
        ss << "      \"text_g\": " << L.text_color[1] << ",\n";
        ss << "      \"text_b\": " << L.text_color[2] << ",\n";
        ss << "      \"text_a\": " << L.text_color[3] << ",\n";
        ss << "      \"text_align\": " << L.text_align << ",\n";
        ss << "      \"font_bold\": " << (L.font_bold ? "true" : "false") << ",\n";
        ss << "      \"font_italic\": " << (L.font_italic ? "true" : "false") << ",\n";
        ss << "      \"font_underline\": " << (L.font_underline ? "true" : "false") << ",\n";
        ss << "      \"font_strikethrough\": " << (L.font_strikethrough ? "true" : "false") << ",\n";
        ss << "      \"line_spacing\": " << L.line_spacing << ",\n";
        ss << "      \"tracking\": " << L.tracking << ",\n";
        ss << "      \"stroke_width\": " << L.stroke_width << ",\n";
        ss << "      \"stroke_r\": " << L.stroke_color[0] << ",\n";
        ss << "      \"stroke_g\": " << L.stroke_color[1] << ",\n";
        ss << "      \"stroke_b\": " << L.stroke_color[2] << ",\n";
        ss << "      \"stroke_a\": " << L.stroke_color[3] << ",\n";
        ss << "      \"bg_r\": " << L.bg_color[0] << ",\n";
        ss << "      \"bg_g\": " << L.bg_color[1] << ",\n";
        ss << "      \"bg_b\": " << L.bg_color[2] << ",\n";
        ss << "      \"bg_a\": " << L.bg_color[3] << ",\n";
        ss << "      \"shadow_enabled\": " << (L.shadow_enabled ? "true" : "false") << ",\n";
        ss << "      \"shadow_offset_x\": " << L.shadow_offset_x << ",\n";
        ss << "      \"shadow_offset_y\": " << L.shadow_offset_y << ",\n";
        ss << "      \"shadow_blur\": " << L.shadow_blur << ",\n";
        ss << "      \"shadow_r\": " << L.shadow_color[0] << ",\n";
        ss << "      \"shadow_g\": " << L.shadow_color[1] << ",\n";
        ss << "      \"shadow_b\": " << L.shadow_color[2] << ",\n";
        ss << "      \"shadow_a\": " << L.shadow_color[3] << "\n";
        ss << "    }" << ((i + 1 < layers.size()) ? "," : "") << "\n";
    }
    ss << "  ]\n}\n";
    return ss.str();
}

static std::vector<std::string> split_json_array_objects(const std::string &json, const std::string &array_key) {
    std::vector<std::string> objects;
    size_t colon = find_json_key(json, array_key);
    if (colon == std::string::npos) return objects;

    size_t bracket = json.find('[', colon + 1);
    if (bracket == std::string::npos) return objects;

    int depth = 0;
    bool in_str = false;
    size_t obj_start = std::string::npos;

    for (size_t i = bracket + 1; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\' && i + 1 < json.size()) {
                ++i;
                continue;
            }
            if (c == '\"') in_str = false;
            continue;
        }
        if (c == '\"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && obj_start != std::string::npos) {
                objects.push_back(json.substr(obj_start, i - obj_start + 1));
                obj_start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

static void parse_layers_json(
    const std::string &layers_str,
    const std::unordered_map<std::string, std::vector<uint8_t>> &layer_blobs,
    std::vector<SerializedLayerEntry> &out_layers
) {
    out_layers.clear();
    auto objs = split_json_array_objects(layers_str, "layers");
    out_layers.reserve(objs.size());

    for (const auto &obj : objs) {
        SerializedLayerEntry L;
        L.id = static_cast<uint32_t>(std::max(0, parse_int_field(obj, "id", 0)));
        L.name = parse_string_field(obj, "name", "Layer");
        L.type = parse_int_field(obj, "type", 0);
        L.visible = parse_bool_field(obj, "visible", true);
        L.blend_mode = parse_int_field(obj, "blend_mode", 0);
        L.opacity = parse_float_field(obj, "opacity", 1.0f);
        L.fill_opacity = parse_float_field(obj, "fill_opacity", 1.0f);
        L.offset_x = parse_float_field(obj, "offset_x", 0.0f);
        L.offset_y = parse_float_field(obj, "offset_y", 0.0f);
        L.rotation_deg = parse_float_field(obj, "rotation_deg", 0.0f);
        L.scale_x = parse_float_field(obj, "scale_x", 1.0f);
        L.scale_y = parse_float_field(obj, "scale_y", 1.0f);
        L.lock_alpha = parse_bool_field(obj, "lock_alpha", false);
        L.lock_pixels = parse_bool_field(obj, "lock_pixels", false);
        L.lock_position = parse_bool_field(obj, "lock_position", false);
        L.width = static_cast<uint32_t>(std::max(0, parse_int_field(obj, "width", 0)));
        L.height = static_cast<uint32_t>(std::max(0, parse_int_field(obj, "height", 0)));
        L.text_utf8 = parse_string_field(obj, "text_utf8");
        L.font_family = parse_string_field(obj, "font_family", "Segoe UI");
        L.font_size = parse_float_field(obj, "font_size", 32.0f);
        L.text_color[0] = parse_float_field(obj, "text_r", 1.0f);
        L.text_color[1] = parse_float_field(obj, "text_g", 1.0f);
        L.text_color[2] = parse_float_field(obj, "text_b", 1.0f);
        L.text_color[3] = parse_float_field(obj, "text_a", 1.0f);
        L.text_align = parse_int_field(obj, "text_align", 0);
        L.font_bold = parse_bool_field(obj, "font_bold", false);
        L.font_italic = parse_bool_field(obj, "font_italic", false);
        L.font_underline = parse_bool_field(obj, "font_underline", false);
        L.font_strikethrough = parse_bool_field(obj, "font_strikethrough", false);
        L.line_spacing = parse_float_field(obj, "line_spacing", 1.15f);
        L.tracking = parse_float_field(obj, "tracking", 0.0f);
        L.stroke_width = parse_float_field(obj, "stroke_width", 0.0f);
        L.stroke_color[0] = parse_float_field(obj, "stroke_r", 0.0f);
        L.stroke_color[1] = parse_float_field(obj, "stroke_g", 0.0f);
        L.stroke_color[2] = parse_float_field(obj, "stroke_b", 0.0f);
        L.stroke_color[3] = parse_float_field(obj, "stroke_a", 1.0f);
        L.bg_color[0] = parse_float_field(obj, "bg_r", 0.0f);
        L.bg_color[1] = parse_float_field(obj, "bg_g", 0.0f);
        L.bg_color[2] = parse_float_field(obj, "bg_b", 0.0f);
        L.bg_color[3] = parse_float_field(obj, "bg_a", 0.0f);
        L.shadow_enabled = parse_bool_field(obj, "shadow_enabled", false);
        L.shadow_offset_x = parse_float_field(obj, "shadow_offset_x", 2.0f);
        L.shadow_offset_y = parse_float_field(obj, "shadow_offset_y", 2.0f);
        L.shadow_blur = parse_float_field(obj, "shadow_blur", 3.0f);
        L.shadow_color[0] = parse_float_field(obj, "shadow_r", 0.0f);
        L.shadow_color[1] = parse_float_field(obj, "shadow_g", 0.0f);
        L.shadow_color[2] = parse_float_field(obj, "shadow_b", 0.0f);
        L.shadow_color[3] = parse_float_field(obj, "shadow_a", 0.6f);

        std::string png_entry = parse_string_field(obj, "png_entry");
        if (!png_entry.empty()) {
            auto it = layer_blobs.find(png_entry);
            if (it != layer_blobs.end()) {
                L.png_bytes = it->second;
            }
        }
        out_layers.push_back(std::move(L));
    }
}

struct ArchiveInputEntry {
    std::string name;
    const void *data = nullptr;
    size_t size = 0;
};

static bool write_zip_archive(
    const std::wstring &project_path,
    const std::vector<ArchiveInputEntry> &archive_entries,
    std::wstring &out_error
) {
    fs::path proj_p(project_path);
    std::error_code ec;
    if (!proj_p.parent_path().empty()) {
        fs::create_directories(proj_p.parent_path(), ec);
    }

    // Write to a sibling temp file first so failed writes never corrupt existing saves.
    fs::path tmp_p = proj_p;
    tmp_p += L".tmp";

    std::ofstream out(tmp_p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        out_error = L"Failed to create temporary project file: " + tmp_p.wstring();
        return false;
    }

    std::vector<InternalZipEntry> entries;
    entries.reserve(archive_entries.size());

    for (const auto &item : archive_entries) {
        uint32_t current_offset = static_cast<uint32_t>(out.tellp());
        uint32_t crc = mz_crc32(0, static_cast<const mz_uint8 *>(item.data), item.size);

        ZipLocalHeader lh;
        lh.signature = 0x04034b50;
        lh.version_needed = 20;
        lh.flags = 0;
        lh.compression = 0; // Stored (fast; images/PNGs are already compressed)
        lh.crc32 = crc;
        lh.comp_size = static_cast<uint32_t>(item.size);
        lh.uncomp_size = static_cast<uint32_t>(item.size);
        lh.name_len = static_cast<uint16_t>(item.name.size());
        lh.extra_len = 0;

        out.write(reinterpret_cast<const char *>(&lh), sizeof(lh));
        out.write(item.name.data(), item.name.size());
        if (item.size > 0 && item.data) {
            out.write(reinterpret_cast<const char *>(item.data), item.size);
        }

        InternalZipEntry entry;
        entry.name = item.name;
        entry.crc32 = crc;
        entry.size = static_cast<uint32_t>(item.size);
        entry.offset = current_offset;
        entries.push_back(entry);
    }

    uint32_t cd_offset = static_cast<uint32_t>(out.tellp());
    for (const auto &e : entries) {
        ZipCentralHeader ch;
        ch.signature = 0x02014b50;
        ch.version_made_by = 20;
        ch.version_needed = 20;
        ch.flags = 0;
        ch.compression = 0;
        ch.crc32 = e.crc32;
        ch.comp_size = e.size;
        ch.uncomp_size = e.size;
        ch.name_len = static_cast<uint16_t>(e.name.size());
        ch.local_header_offset = e.offset;

        out.write(reinterpret_cast<const char *>(&ch), sizeof(ch));
        out.write(e.name.data(), e.name.size());
    }
    uint32_t cd_end = static_cast<uint32_t>(out.tellp());
    uint32_t cd_size = cd_end - cd_offset;

    ZipEndOfCentralDir eocd;
    eocd.signature = 0x06054b50;
    eocd.disk_number = 0;
    eocd.central_dir_disk = 0;
    eocd.records_on_disk = static_cast<uint16_t>(entries.size());
    eocd.total_records = static_cast<uint16_t>(entries.size());
    eocd.central_dir_size = cd_size;
    eocd.central_dir_offset = cd_offset;
    eocd.comment_len = 0;

    out.write(reinterpret_cast<const char *>(&eocd), sizeof(eocd));
    out.flush();
    bool write_ok = out.good();
    out.close();

    if (!write_ok) {
        fs::remove(tmp_p, ec);
        out_error = L"Failed while writing project archive";
        return false;
    }

    fs::rename(tmp_p, proj_p, ec);
    if (ec) {
        fs::remove(proj_p, ec);
        ec.clear();
        fs::rename(tmp_p, proj_p, ec);
        if (ec) {
            fs::remove(tmp_p, ec);
            out_error = L"Failed to finalize project file: " + project_path;
            return false;
        }
    }

    return true;
}

static bool save_project_common(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    const std::vector<SerializedLayerEntry> *layers_opt,
    uint32_t canvas_width,
    uint32_t canvas_height,
    int active_layer_index,
    std::wstring &out_error
) {
    if (project_path.empty()) {
        out_error = L"Target project path is empty";
        return false;
    }

    fs::path src_p(source_image_path);
    std::vector<uint8_t> src_image_bytes;
    if (!source_image_path.empty() && fs::exists(src_p)) {
        if (!read_entire_file(src_p, src_image_bytes)) {
            out_error = L"Could not read source image: " + source_image_path;
            return false;
        }
    }

    std::vector<uint8_t> depth_bytes;
    if (depth_state.has_depth && !depth_state.depth_sidecar_path.empty()) {
        fs::path depth_p(depth_state.depth_sidecar_path);
        if (fs::exists(depth_p)) {
            read_entire_file(depth_p, depth_bytes);
        }
    }

    const bool is_v2 = (layers_opt != nullptr);
    const std::string format_ver = is_v2 ? "2.0" : "1.0";
    std::string orig_img_name = src_p.filename().string();
    if (orig_img_name.empty() && is_v2) {
        orig_img_name = fs::path(project_path).stem().string() + ".png";
    }

    uint32_t layer_count = is_v2 ? static_cast<uint32_t>(layers_opt->size()) : 0;
    std::string manifest_json = build_manifest_json(
        format_ver, orig_img_name, view_state, depth_state,
        canvas_width, canvas_height, layer_count, active_layer_index
    );

    std::vector<ArchiveInputEntry> archive;
    archive.push_back({ "manifest.json", manifest_json.data(), manifest_json.size() });

    if (!preset_ini_bytes.empty()) {
        archive.push_back({ "preset.ini", preset_ini_bytes.data(), preset_ini_bytes.size() });
    }

    std::string src_entry_name;
    if (!src_image_bytes.empty()) {
        src_entry_name = "source_image" + src_p.extension().string();
        archive.push_back({ src_entry_name, src_image_bytes.data(), src_image_bytes.size() });
    }

    if (!depth_bytes.empty()) {
        archive.push_back({ "depth.sldepth", depth_bytes.data(), depth_bytes.size() });
    }

    std::string layers_json_str;
    std::vector<std::string> png_entry_names;
    if (is_v2) {
        png_entry_names.resize(layers_opt->size());
        for (size_t i = 0; i < layers_opt->size(); ++i) {
            if (!(*layers_opt)[i].png_bytes.empty()) {
                png_entry_names[i] = "layers/layer_" + std::to_string(i) + ".png";
            }
        }
        layers_json_str = build_layers_json(*layers_opt, png_entry_names);
        archive.push_back({ "layers.json", layers_json_str.data(), layers_json_str.size() });

        for (size_t i = 0; i < layers_opt->size(); ++i) {
            if (!png_entry_names[i].empty()) {
                const auto &bytes = (*layers_opt)[i].png_bytes;
                archive.push_back({ png_entry_names[i], bytes.data(), bytes.size() });
            }
        }
    }

    return write_zip_archive(project_path, archive, out_error);
}

bool save_project(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    std::wstring &out_error
) {
    return save_project_common(
        project_path, source_image_path, preset_ini_bytes,
        view_state, depth_state, nullptr, 0, 0, 0, out_error
    );
}

bool save_project_v2(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    const std::vector<SerializedLayerEntry> &layers,
    uint32_t canvas_width,
    uint32_t canvas_height,
    int active_layer_index,
    std::wstring &out_error
) {
    return save_project_common(
        project_path, source_image_path, preset_ini_bytes,
        view_state, depth_state, &layers, canvas_width, canvas_height, active_layer_index, out_error
    );
}

bool load_project(
    const std::wstring &project_path,
    const std::wstring &temp_extract_dir,
    std::wstring &out_image_path,
    std::wstring &out_preset_path,
    ProjectManifest &out_manifest,
    std::wstring &out_error
) {
    std::vector<SerializedLayerEntry> unused_layers;
    return load_project_v2(
        project_path, temp_extract_dir, out_image_path, out_preset_path,
        out_manifest, unused_layers, out_error
    );
}

bool load_project_v2(
    const std::wstring &project_path,
    const std::wstring &temp_extract_dir,
    std::wstring &out_image_path,
    std::wstring &out_preset_path,
    ProjectManifest &out_manifest,
    std::vector<SerializedLayerEntry> &out_layers,
    std::wstring &out_error
) {
    out_layers.clear();
    out_image_path.clear();
    out_preset_path.clear();

    if (!fs::exists(project_path)) {
        out_error = L"Project file does not exist: " + project_path;
        return false;
    }

    std::ifstream in(project_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        out_error = L"Could not open project file: " + project_path;
        return false;
    }

    std::streamsize file_size = in.tellg();
    if (file_size < static_cast<std::streamsize>(sizeof(ZipEndOfCentralDir))) {
        out_error = L"Invalid project file (too small)";
        return false;
    }

    // scan backwards from EOF to find EOCD signature 0x06054b50
    size_t scan_size = std::min(static_cast<size_t>(file_size), static_cast<size_t>(65536));
    std::vector<uint8_t> tail(scan_size);
    in.seekg(file_size - scan_size, std::ios::beg);
    in.read(reinterpret_cast<char *>(tail.data()), scan_size);

    int eocd_pos = -1;
    for (int i = static_cast<int>(scan_size - sizeof(ZipEndOfCentralDir)); i >= 0; --i) {
        if (*reinterpret_cast<uint32_t *>(&tail[i]) == 0x06054b50) {
            eocd_pos = i;
            break;
        }
    }

    if (eocd_pos < 0) {
        out_error = L"Invalid .shaderlab file: central directory not found";
        return false;
    }

    ZipEndOfCentralDir eocd;
    std::memcpy(&eocd, &tail[eocd_pos], sizeof(eocd));
    if (static_cast<std::streamsize>(eocd.central_dir_offset) >= file_size) {
        out_error = L"Invalid .shaderlab file: bad central directory offset";
        return false;
    }

    fs::path extract_p(temp_extract_dir);
    std::error_code ec;
    fs::create_directories(extract_p, ec);

    // Pass 1: find manifest.json and layers.json before writing sidecar payloads.
    std::string manifest_str;
    std::string layers_str;
    in.seekg(eocd.central_dir_offset, std::ios::beg);
    for (uint16_t i = 0; i < eocd.total_records; ++i) {
        ZipCentralHeader ch;
        in.read(reinterpret_cast<char *>(&ch), sizeof(ch));
        if (!in.good() || ch.signature != 0x02014b50) break;

        std::string fname(ch.name_len, '\0');
        in.read(&fname[0], ch.name_len);
        if (ch.extra_len > 0) in.seekg(ch.extra_len, std::ios::cur);
        if (ch.comment_len > 0) in.seekg(ch.comment_len, std::ios::cur);

        std::streampos next_cd = in.tellg();

        if (fname == "manifest.json" || fname == "layers.json") {
            if (static_cast<std::streamsize>(ch.local_header_offset) < file_size) {
                in.seekg(ch.local_header_offset, std::ios::beg);
                ZipLocalHeader lh;
                in.read(reinterpret_cast<char *>(&lh), sizeof(lh));
                if (lh.signature == 0x04034b50) {
                    in.seekg(lh.name_len + lh.extra_len, std::ios::cur);
                    std::string buf(lh.uncomp_size, '\0');
                    if (lh.uncomp_size > 0) {
                        in.read(&buf[0], lh.uncomp_size);
                    }
                    if (fname == "manifest.json") {
                        manifest_str = std::move(buf);
                    } else {
                        layers_str = std::move(buf);
                    }
                }
            }
        }
        in.seekg(next_cd, std::ios::beg);
    }

    if (!manifest_str.empty()) {
        std::string fmt = parse_string_field(manifest_str, "format_version");
        if (!fmt.empty()) out_manifest.format_version = fmt;
        out_manifest.original_image_name = parse_string_field(manifest_str, "original_image_name");
        out_manifest.image_width = static_cast<uint32_t>(std::max(0, parse_int_field(manifest_str, "image_width", 0)));
        out_manifest.image_height = static_cast<uint32_t>(std::max(0, parse_int_field(manifest_str, "image_height", 0)));
        out_manifest.layer_count = static_cast<uint32_t>(std::max(0, parse_int_field(manifest_str, "layer_count", 0)));
        out_manifest.active_layer_index = parse_int_field(manifest_str, "active_layer_index", 0);
        out_manifest.view.zoom = parse_float_field(manifest_str, "zoom", 1.0f);
        out_manifest.view.angle = parse_float_field(manifest_str, "angle", 0.0f);
        out_manifest.view.pan[0] = parse_float_field(manifest_str, "pan_x", 0.0f);
        out_manifest.view.pan[1] = parse_float_field(manifest_str, "pan_y", 0.0f);
        out_manifest.view.is_fullscreen = parse_bool_field(manifest_str, "is_fullscreen", false);
        out_manifest.view.lock_pan = parse_bool_field(manifest_str, "lock_pan", false);
        out_manifest.view.lock_zoom = parse_bool_field(manifest_str, "lock_zoom", false);
        out_manifest.view.lock_rot = parse_bool_field(manifest_str, "lock_rot", false);
        out_manifest.depth.has_depth = parse_bool_field(manifest_str, "has_depth", false);
        out_manifest.depth.far_plane = parse_float_field(manifest_str, "far_plane", 1000.0f);
    }

    // Pass 2: extract disk sidecars and read layer PNG payloads into memory.
    in.seekg(eocd.central_dir_offset, std::ios::beg);
    fs::path extracted_image_file;
    std::unordered_map<std::string, std::vector<uint8_t>> layer_blobs;

    for (uint16_t i = 0; i < eocd.total_records; ++i) {
        ZipCentralHeader ch;
        in.read(reinterpret_cast<char *>(&ch), sizeof(ch));
        if (!in.good() || ch.signature != 0x02014b50) break;

        std::string fname(ch.name_len, '\0');
        in.read(&fname[0], ch.name_len);
        if (ch.extra_len > 0) in.seekg(ch.extra_len, std::ios::cur);
        if (ch.comment_len > 0) in.seekg(ch.comment_len, std::ios::cur);

        std::streampos next_cd = in.tellg();

        if (!is_safe_zip_entry(fname) || static_cast<std::streamsize>(ch.local_header_offset) >= file_size) {
            in.seekg(next_cd, std::ios::beg);
            continue;
        }

        in.seekg(ch.local_header_offset, std::ios::beg);
        ZipLocalHeader lh;
        in.read(reinterpret_cast<char *>(&lh), sizeof(lh));
        if (lh.signature != 0x04034b50) {
            in.seekg(next_cd, std::ios::beg);
            continue;
        }
        in.seekg(lh.name_len + lh.extra_len, std::ios::cur);

        std::vector<uint8_t> payload(lh.uncomp_size);
        if (lh.uncomp_size > 0) {
            in.read(reinterpret_cast<char *>(payload.data()), lh.uncomp_size);
        }

        if (fname == "layers.json") {
            in.seekg(next_cd, std::ios::beg);
            continue;
        }
        if (fname.rfind("layers/", 0) == 0) {
            layer_blobs[fname] = std::move(payload);
            in.seekg(next_cd, std::ios::beg);
            continue;
        }

        fs::path safe_fname = fs::path(fname).filename();
        if (safe_fname.empty() || safe_fname == "." || safe_fname == "..") {
            in.seekg(next_cd, std::ios::beg);
            continue;
        }

        fs::path dest_file;
        if (fname.rfind("source_image", 0) == 0) {
            std::string save_name = out_manifest.original_image_name.empty()
                ? safe_fname.string()
                : fs::path(out_manifest.original_image_name).filename().string();
            dest_file = extract_p / save_name;
            extracted_image_file = dest_file;
            out_image_path = dest_file.wstring();
        } else if (fname == "preset.ini") {
            std::string preset_stem;
            if (!out_manifest.original_image_name.empty()) {
                preset_stem = fs::path(out_manifest.original_image_name).filename().stem().string();
            } else {
                preset_stem = fs::path(project_path).filename().stem().string();
            }
            if (preset_stem.empty()) preset_stem = "ShaderLab_Preset";
            dest_file = extract_p / (preset_stem + ".ini");
            out_preset_path = dest_file.wstring();
        } else if (fname == "depth.sldepth") {
            std::wstring depth_name = L"depth.sldepth";
            if (!out_manifest.original_image_name.empty()) {
                depth_name = fs::path(out_manifest.original_image_name).filename().stem().wstring() + L".sldepth";
            } else if (!extracted_image_file.empty()) {
                depth_name = extracted_image_file.filename().stem().wstring() + L".sldepth";
            }
            dest_file = extract_p / depth_name;
            out_manifest.depth.depth_sidecar_path = dest_file.wstring();
        } else {
            dest_file = extract_p / safe_fname;
        }

        std::ofstream out_f(dest_file, std::ios::binary);
        if (out_f.is_open() && !payload.empty()) {
            out_f.write(reinterpret_cast<const char *>(payload.data()), payload.size());
        }

        in.seekg(next_cd, std::ios::beg);
    }

    if (!layers_str.empty()) {
        parse_layers_json(layers_str, layer_blobs, out_layers);
    }

    return true;
}

} // namespace project_file
