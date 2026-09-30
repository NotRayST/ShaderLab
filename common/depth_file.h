#pragma once
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t kSidecarMagic = 0x534C4431; // 'SLD1'
constexpr uint16_t kSidecarVersion = 1;

constexpr uint32_t kSidecarFlagValid = 1 << 0;
constexpr uint32_t kSidecarFlagFlat  = 1 << 1;

#pragma pack(push, 1)
struct SidecarDepthHeader {
    uint32_t magic;                // 'SLD1' = 0x534C4431
    uint16_t version;              // 1
    uint16_t encoding;             // 0 = R32F, 1 = R16U, 2 = R8U, 3 = R4U (Deflate compressed)
    uint32_t width;                // width in pixels
    uint32_t height;               // height in pixels
    uint32_t flags;                // kSidecarFlagValid, etc.
    float far_plane_used;          // e.g. 1000.0f
    uint32_t raw_byte_size;        // width * height * 4
    uint32_t compressed_byte_size; // size of deflated payload
    uint64_t timestamp;            // epoch ms
    char game_name[64];            // game title or identifier
};
#pragma pack(pop)

namespace depth_file {

// clamps to [0, 1], replaces nans, checks for flat/blank depth
bool sanitize_and_analyze(float *linear_depth, size_t count, bool &is_flat, float &min_val, float &max_val);

// writes .sldepth sidecar (header + deflated r32f + crc32)
bool write_sidecar(
    const std::string &path,
    const SidecarDepthHeader &header,
    const float *linear_depth,
    size_t count,
    std::string &error
);

// reads and inflates .sldepth sidecar
bool read_sidecar(
    const std::string &path,
    SidecarDepthHeader &header,
    std::vector<float> &linear_depth,
    std::string &error
);

} // namespace depth_file
