#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

#pragma pack(push, 1)
struct DepthMapHeader {
    uint32_t magic;                 // 0x534C4431 ('SLD1')
    uint16_t version;               // 1
    uint16_t encoding;              // 0 = R32F_Deflate, 1 = R16U_Deflate, 2 = R8U_Deflate, 3 = R4U_Deflate
    uint32_t width;                 // Depth width in pixels
    uint32_t height;                // Depth height in pixels
    uint32_t flags;                 // bit0: depth_valid (1 = valid non-flat gradient, 0 = dummy/flat)
    float    near_plane;            // Normalized near plane (1.0)
    float    far_plane;             // Virtual linearization far plane (e.g. 1000.0)
    uint32_t raw_byte_size;         // Uncompressed payload size in bytes
    uint32_t compressed_byte_size;  // Compressed payload size in bytes
    uint64_t timestamp;             // Unix timestamp in ms
    char     game_name[64];         // Captured game / process name
};
#pragma pack(pop)

static constexpr uint32_t kDepthMagicSLD1 = 0x534C4431; // 'SLD1'
static constexpr uint16_t kDepthVersion1   = 1;
static constexpr uint32_t kDepthFlagValid  = (1 << 0);

namespace depth_chunk {

uint32_t calc_crc32(const uint8_t *data, size_t len);

bool extract_sldp(
    const uint8_t *png_data,
    size_t size,
    DepthMapHeader &out_header,
    std::vector<float> &out_floats,
    std::string &out_error
);

bool splice_sldp_bytes(
    const uint8_t *in_png,
    size_t in_size,
    const DepthMapHeader &header,
    const float *depth_floats,
    size_t count,
    std::vector<uint8_t> &out_png,
    std::string &out_error
);

bool inject_sldp(
    const std::string &png_path,
    const DepthMapHeader &header,
    const float *depth_floats,
    size_t count,
    std::string &out_error
);

} // namespace depth_chunk
