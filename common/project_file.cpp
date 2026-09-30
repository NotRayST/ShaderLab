#include "project_file.h"
#include "../third_party/miniz/miniz.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <cctype>

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

static std::string build_manifest_json(
    const std::string &orig_img_name,
    const ViewportState &v,
    const DepthState &d
) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"format_version\": \"1.0\",\n";
    ss << "  \"app_version\": \"v1.2.2\",\n";
    ss << "  \"timestamp\": " << now_ms << ",\n";
    ss << "  \"original_image_name\": \"" << orig_img_name << "\",\n";
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

static size_t find_json_key(const std::string &json, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t colon = pos + needle.length();
        while (colon < json.length() && (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n')) {
            colon++;
        }
        if (colon < json.length() && json[colon] == ':') {
            return colon;
        }
        pos += needle.length();
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

static bool parse_bool_field(const std::string &json, const std::string &key, bool default_val) {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return default_val;
    size_t end = json.find_first_of(",}\n\r", colon + 1);
    std::string val_part = json.substr(colon + 1, (end == std::string::npos) ? std::string::npos : (end - (colon + 1)));
    if (val_part.find("true") != std::string::npos) return true;
    if (val_part.find("false") != std::string::npos) return false;
    return default_val;
}

static std::string parse_string_field(const std::string &json, const std::string &key) {
    size_t colon = find_json_key(json, key);
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('\"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('\"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

bool save_project(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    std::wstring &out_error
) {
    if (project_path.empty()) {
        out_error = L"Target project path is empty";
        return false;
    }

    fs::path proj_p(project_path);
    std::error_code ec;
    fs::create_directories(proj_p.parent_path(), ec);

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

    std::string orig_img_name = src_p.filename().string();
    std::string manifest_json = build_manifest_json(orig_img_name, view_state, depth_state);

    std::ofstream out(proj_p, std::ios::binary);
    if (!out.is_open()) {
        out_error = L"Failed to create project file: " + project_path;
        return false;
    }

    std::vector<InternalZipEntry> entries;

    auto write_zip_file = [&](const std::string &entry_name, const void *data_ptr, size_t data_sz) {
        uint32_t current_offset = static_cast<uint32_t>(out.tellp());
        uint32_t crc = mz_crc32(0, static_cast<const mz_uint8 *>(data_ptr), data_sz);

        ZipLocalHeader lh;
        lh.signature = 0x04034b50;
        lh.version_needed = 20;
        lh.flags = 0;
        lh.compression = 0; // Stored
        lh.crc32 = crc;
        lh.comp_size = static_cast<uint32_t>(data_sz);
        lh.uncomp_size = static_cast<uint32_t>(data_sz);
        lh.name_len = static_cast<uint16_t>(entry_name.size());
        lh.extra_len = 0;

        out.write(reinterpret_cast<const char *>(&lh), sizeof(lh));
        out.write(entry_name.data(), entry_name.size());
        if (data_sz > 0) {
            out.write(reinterpret_cast<const char *>(data_ptr), data_sz);
        }

        InternalZipEntry entry;
        entry.name = entry_name;
        entry.crc32 = crc;
        entry.size = static_cast<uint32_t>(data_sz);
        entry.offset = current_offset;
        entries.push_back(entry);
    };

    write_zip_file("manifest.json", manifest_json.data(), manifest_json.size());

    if (!preset_ini_bytes.empty()) {
        write_zip_file("preset.ini", preset_ini_bytes.data(), preset_ini_bytes.size());
    }

    if (!src_image_bytes.empty()) {
        std::string src_entry = "source_image" + src_p.extension().string();
        write_zip_file(src_entry, src_image_bytes.data(), src_image_bytes.size());
    }

    if (!depth_bytes.empty()) {
        write_zip_file("depth.sldepth", depth_bytes.data(), depth_bytes.size());
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

    if (!out.good()) {
        out_error = L"Failed while writing project archive";
        return false;
    }

    return true;
}

bool load_project(
    const std::wstring &project_path,
    const std::wstring &temp_extract_dir,
    std::wstring &out_image_path,
    std::wstring &out_preset_path,
    ProjectManifest &out_manifest,
    std::wstring &out_error
) {
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
    if (file_size < sizeof(ZipEndOfCentralDir)) {
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

    fs::path extract_p(temp_extract_dir);
    std::error_code ec;
    fs::create_directories(extract_p, ec);

    // pass 1: find and read manifest.json first to know original_image_name
    std::string manifest_str;
    in.seekg(eocd.central_dir_offset, std::ios::beg);
    for (uint16_t i = 0; i < eocd.total_records; ++i) {
        ZipCentralHeader ch;
        in.read(reinterpret_cast<char *>(&ch), sizeof(ch));
        if (ch.signature != 0x02014b50) break;

        std::string fname(ch.name_len, '\0');
        in.read(&fname[0], ch.name_len);
        if (ch.extra_len > 0) in.seekg(ch.extra_len, std::ios::cur);
        if (ch.comment_len > 0) in.seekg(ch.comment_len, std::ios::cur);

        std::streampos next_cd = in.tellg();

        if (fname == "manifest.json") {
            in.seekg(ch.local_header_offset, std::ios::beg);
            ZipLocalHeader lh;
            in.read(reinterpret_cast<char *>(&lh), sizeof(lh));
            in.seekg(lh.name_len + lh.extra_len, std::ios::cur);

            manifest_str.resize(lh.uncomp_size);
            if (lh.uncomp_size > 0) {
                in.read(&manifest_str[0], lh.uncomp_size);
            }
            break;
        }
        in.seekg(next_cd, std::ios::beg);
    }

    if (!manifest_str.empty()) {
        out_manifest.original_image_name = parse_string_field(manifest_str, "original_image_name");
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

    // pass 2: extract all payload files
    in.seekg(eocd.central_dir_offset, std::ios::beg);
    fs::path extracted_image_file;

    for (uint16_t i = 0; i < eocd.total_records; ++i) {
        ZipCentralHeader ch;
        in.read(reinterpret_cast<char *>(&ch), sizeof(ch));
        if (ch.signature != 0x02014b50) break;

        std::string fname(ch.name_len, '\0');
        in.read(&fname[0], ch.name_len);
        if (ch.extra_len > 0) in.seekg(ch.extra_len, std::ios::cur);
        if (ch.comment_len > 0) in.seekg(ch.comment_len, std::ios::cur);

        std::streampos next_cd = in.tellg();

        in.seekg(ch.local_header_offset, std::ios::beg);
        ZipLocalHeader lh;
        in.read(reinterpret_cast<char *>(&lh), sizeof(lh));
        in.seekg(lh.name_len + lh.extra_len, std::ios::cur);

        std::vector<uint8_t> payload(lh.uncomp_size);
        if (lh.uncomp_size > 0) {
            in.read(reinterpret_cast<char *>(payload.data()), lh.uncomp_size);
        }

        fs::path safe_fname = fs::path(fname).filename();
        if (safe_fname.empty() || safe_fname == "." || safe_fname == "..") {
            in.seekg(next_cd, std::ios::beg);
            continue;
        }

        fs::path dest_file;
        if (fname.rfind("source_image", 0) == 0) {
            std::string save_name = out_manifest.original_image_name.empty() ? safe_fname.string() : fs::path(out_manifest.original_image_name).filename().string();
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

    return true;
}

} // namespace project_file
