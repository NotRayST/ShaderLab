#include "depth_file.h"
#include "../third_party/miniz/miniz.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace depth_file {

bool sanitize_and_analyze(float *linear_depth, size_t count, bool &is_flat, float &min_val, float &max_val) {
    if (!linear_depth || count == 0) {
        is_flat = true;
        min_val = 1.0f;
        max_val = 1.0f;
        return false;
    }

    min_val = 1.0f;
    max_val = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        float val = linear_depth[i];
        if (std::isnan(val) || std::isinf(val)) {
            val = 1.0f;
        } else {
            val = std::clamp(val, 0.0f, 1.0f);
        }
        linear_depth[i] = val;

        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
    }

    // flat if delta between min/max is negligible
    is_flat = (max_val - min_val) < 1e-4f;
    return true;
}

bool write_sidecar(
    const std::string &path,
    const SidecarDepthHeader &header,
    const float *linear_depth,
    size_t count,
    std::string &error)
{
    if (!linear_depth || count == 0) {
        error = "Invalid depth data";
        return false;
    }

    size_t raw_bytes = 0;
    if (header.encoding == 1) {
        raw_bytes = count * sizeof(uint16_t);
    } else if (header.encoding == 2) {
        raw_bytes = count * sizeof(uint8_t);
    } else if (header.encoding == 3) {
        raw_bytes = (count + 1) / 2;
    } else {
        raw_bytes = count * sizeof(float);
    }

    std::vector<uint16_t> u16_buf;
    std::vector<uint8_t> u8_buf;
    const void *src_ptr = linear_depth;

    if (header.encoding == 1) {
        uint32_t width = header.width;
        uint32_t height = header.height;
        u16_buf.resize(count);
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t row_start = y * width;
            uint16_t prev = 0;
            for (uint32_t x = 0; x < width; ++x) {
                float v = std::clamp(linear_depth[row_start + x], 0.0f, 1.0f);
                uint16_t curr = static_cast<uint16_t>(v * 65535.0f + 0.5f);
                u16_buf[row_start + x] = static_cast<uint16_t>(static_cast<uint32_t>(curr) - static_cast<uint32_t>(prev));
                prev = curr;
            }
        }
        src_ptr = u16_buf.data();
    } else if (header.encoding == 2) {
        uint32_t width = header.width;
        uint32_t height = header.height;
        u8_buf.resize(count);
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t row_start = y * width;
            uint8_t prev = 0;
            for (uint32_t x = 0; x < width; ++x) {
                float v = std::clamp(linear_depth[row_start + x], 0.0f, 1.0f);
                uint8_t curr = static_cast<uint8_t>(v * 255.0f + 0.5f);
                u8_buf[row_start + x] = static_cast<uint8_t>(static_cast<uint32_t>(curr) - static_cast<uint32_t>(prev));
                prev = curr;
            }
        }
        src_ptr = u8_buf.data();
    } else if (header.encoding == 3) {
        u8_buf.assign(raw_bytes, 0);
        for (size_t i = 0; i < count; ++i) {
            float v = std::clamp(linear_depth[i], 0.0f, 1.0f);
            uint8_t q = static_cast<uint8_t>(v * 15.0f + 0.5f) & 0x0F;
            if (i % 2 == 0) {
                u8_buf[i / 2] |= (q << 4);
            } else {
                u8_buf[i / 2] |= q;
            }
        }
        src_ptr = u8_buf.data();
    }

    mz_ulong comp_bound = mz_compressBound(static_cast<mz_ulong>(raw_bytes));
    std::vector<uint8_t> compressed_data(comp_bound);
    mz_ulong comp_len = comp_bound;

    int res = mz_compress(
        compressed_data.data(),
        &comp_len,
        reinterpret_cast<const unsigned char *>(src_ptr),
        static_cast<mz_ulong>(raw_bytes)
    );

    if (res != MZ_OK) {
        error = "Compression failed with code " + std::to_string(res);
        return false;
    }

    compressed_data.resize(comp_len);

    SidecarDepthHeader out_header = header;
    out_header.magic = kSidecarMagic;
    out_header.version = kSidecarVersion;
    out_header.encoding = header.encoding;
    out_header.raw_byte_size = static_cast<uint32_t>(raw_bytes);
    out_header.compressed_byte_size = static_cast<uint32_t>(comp_len);

    // crc32 over header + compressed payload
    mz_uint32 crc = mz_crc32(0, reinterpret_cast<const mz_uint8 *>(&out_header), sizeof(out_header));
    crc = mz_crc32(crc, compressed_data.data(), compressed_data.size());

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        error = "Failed to open output file: " + path;
        return false;
    }

    out.write(reinterpret_cast<const char *>(&out_header), sizeof(out_header));
    out.write(reinterpret_cast<const char *>(compressed_data.data()), compressed_data.size());
    out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));

    if (!out.good()) {
        error = "Failed to write complete sidecar file";
        return false;
    }

    return true;
}

bool read_sidecar(
    const std::string &path,
    SidecarDepthHeader &header,
    std::vector<float> &linear_depth,
    std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "Failed to open sidecar file: " + path;
        return false;
    }

    SidecarDepthHeader in_header = {};
    in.read(reinterpret_cast<char *>(&in_header), sizeof(in_header));
    if (in.gcount() != sizeof(in_header)) {
        error = "Failed to read sidecar header (file truncated)";
        return false;
    }

    if (in_header.magic != kSidecarMagic || in_header.version != kSidecarVersion) {
        error = "Invalid sidecar magic or version";
        return false;
    }

    if (in_header.compressed_byte_size == 0 || in_header.raw_byte_size == 0) {
        error = "Invalid sidecar payload dimensions";
        return false;
    }

    std::vector<uint8_t> compressed_data(in_header.compressed_byte_size);
    in.read(reinterpret_cast<char *>(compressed_data.data()), in_header.compressed_byte_size);
    if (in.gcount() != in_header.compressed_byte_size) {
        error = "Failed to read compressed payload (file truncated)";
        return false;
    }

    mz_uint32 file_crc = 0;
    in.read(reinterpret_cast<char *>(&file_crc), sizeof(file_crc));
    if (in.gcount() != sizeof(file_crc)) {
        error = "Missing CRC32 trailer";
        return false;
    }

    mz_uint32 calc_crc = mz_crc32(0, reinterpret_cast<const mz_uint8 *>(&in_header), sizeof(in_header));
    calc_crc = mz_crc32(calc_crc, compressed_data.data(), compressed_data.size());

    if (calc_crc != file_crc) {
        error = "CRC32 checksum mismatch (file corrupt)";
        return false;
    }

    header = in_header;
    mz_ulong uncomp_len = in_header.raw_byte_size;

    if (in_header.encoding == 1) {
        size_t count = in_header.raw_byte_size / sizeof(uint16_t);
        std::vector<uint16_t> u16_data(count);
        int res = mz_uncompress(
            reinterpret_cast<unsigned char *>(u16_data.data()),
            &uncomp_len,
            compressed_data.data(),
            in_header.compressed_byte_size
        );
        if (res != MZ_OK || uncomp_len != in_header.raw_byte_size) {
            error = "Decompression failed";
            return false;
        }
        linear_depth.resize(count);
        uint32_t width = in_header.width;
        uint32_t height = in_header.height;
        if (width > 0 && height > 0 && width * height == count) {
            for (uint32_t y = 0; y < height; ++y) {
                uint32_t row_start = y * width;
                uint16_t running = 0;
                for (uint32_t x = 0; x < width; ++x) {
                    running = static_cast<uint16_t>(static_cast<uint32_t>(running) + static_cast<uint32_t>(u16_data[row_start + x]));
                    linear_depth[row_start + x] = running / 65535.0f;
                }
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                linear_depth[i] = u16_data[i] / 65535.0f;
            }
        }
    } else if (in_header.encoding == 2) {
        size_t count = in_header.raw_byte_size / sizeof(uint8_t);
        std::vector<uint8_t> u8_data(count);
        int res = mz_uncompress(
            reinterpret_cast<unsigned char *>(u8_data.data()),
            &uncomp_len,
            compressed_data.data(),
            in_header.compressed_byte_size
        );
        if (res != MZ_OK || uncomp_len != in_header.raw_byte_size) {
            error = "Decompression failed";
            return false;
        }
        linear_depth.resize(count);
        uint32_t width = in_header.width;
        uint32_t height = in_header.height;
        if (width > 0 && height > 0 && width * height == count) {
            for (uint32_t y = 0; y < height; ++y) {
                uint32_t row_start = y * width;
                uint8_t running = 0;
                for (uint32_t x = 0; x < width; ++x) {
                    running = static_cast<uint8_t>(static_cast<uint32_t>(running) + static_cast<uint32_t>(u8_data[row_start + x]));
                    linear_depth[row_start + x] = running / 255.0f;
                }
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                linear_depth[i] = u8_data[i] / 255.0f;
            }
        }
    } else if (in_header.encoding == 3) {
        uint32_t width = in_header.width;
        uint32_t height = in_header.height;
        size_t count = (width > 0 && height > 0) ? (static_cast<size_t>(width) * height) : (in_header.raw_byte_size * 2);
        std::vector<uint8_t> u4_data(in_header.raw_byte_size);
        int res = mz_uncompress(
            reinterpret_cast<unsigned char *>(u4_data.data()),
            &uncomp_len,
            compressed_data.data(),
            in_header.compressed_byte_size
        );
        if (res != MZ_OK || uncomp_len != in_header.raw_byte_size) {
            error = "Decompression failed";
            return false;
        }
        linear_depth.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint8_t b = u4_data[i / 2];
            uint8_t q = (i % 2 == 0) ? ((b >> 4) & 0x0F) : (b & 0x0F);
            linear_depth[i] = q / 15.0f;
        }
    } else {
        size_t count = in_header.raw_byte_size / sizeof(float);
        linear_depth.resize(count);
        int res = mz_uncompress(
            reinterpret_cast<unsigned char *>(linear_depth.data()),
            &uncomp_len,
            compressed_data.data(),
            in_header.compressed_byte_size
        );
        if (res != MZ_OK || uncomp_len != in_header.raw_byte_size) {
            error = "Decompression failed";
            return false;
        }
    }

    return true;
}

} // namespace depth_file
